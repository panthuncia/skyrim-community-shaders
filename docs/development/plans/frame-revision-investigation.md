# One central system for resizes and graph changes

Investigation, 2026-11-18. Worktree `org-port-worktrees/test-runner`, CS branch `org-native/phases` at `e6b0f078b`
("apply graph texture sizes at Begin"). Read-only: nothing was built or run.

Paths are relative to the worktree. `DCLF/` is `src/Features/DrawcallLimitFix/`. `ORG/` is `extern/OpenRenderGraph/`. `OMS/` is
`extern/ORGModuleServices/include/ORGModuleServices/`. Citations are `file:function`. Line numbers are left out on purpose.
Anything marked **(inferred)** I concluded but did not read directly.

## The goal, restated

- Inputs are **variables**: render size, dynamic resolution ratios, upscaler method, display size, HDR, feature toggles, the settings
  that set sizes or pass sets, and so on.
- A change to a variable is an **event**.
- Resources declare which variables decide their size, format and existence.
- An event resizes the dependent resources off the render path. That cascades into async rebuilds and re-records of the graphs
  that use them.
- The frame's Begin **adopts the latest fully prepared set**. Nothing inside a frame asks "do I need to resize", and nothing inside a
  frame queues work for the next one.

Today DCLF already works this way for its own scene data. The rest of the frame does not. It polls in-frame and patches at the next
Begin.

---

## 1. How DCLF does it today

DCLF has one pipeline:

1. Engine writers push **events**.
2. The **scene lane** applies them and publishes a `ScenePublication`.
3. The **snapshot builder** turns the newest publication into a **revision**. A revision is requirements, then a seal (or a growth
   batch if something is too small), then shapes, then recordings.
4. The builder posts one **`SceneSnapshot`**.
5. The **frame start** adopts that snapshot whole.

Graph rebuilds run on a separate track. They make every older snapshot stale by build generation.

### 1.1 Event sources

| Source | Where it enters | Where it is consumed |
|---|---|---|
| SceneWriterFramework events: attach, detach, move, switch index/structural, fade reset, hidden, scene write, written, engine writer calls (`SWF_EVENT_CALL(SWF_HOOK_*)`) | `DCLF/Core/Engine/Capture/SceneWriter.cpp:SceneWriter::Subscribe`; handlers in `SceneTracker.cpp:OnChildAttached/OnChildDetaching`, `SceneEvents.cpp:OnMoved/OnSwitchIndex/OnSceneWrite/...`, `Features/HiddenNodes/HiddenStores.cpp:OnHidden`, and per feature (`ActorEvents.cpp`, `DecalHooks.cpp`, `FadeEvents.cpp`, `LodSegmentHooks.cpp`, ...) | Each handler pushes into a lock-free `EventQueue` (`Core/Runtime/EventQueue.h`) and calls `WakeScenePass`. The scene lane (`Core/Scene/Coordinator/ScenePump.cpp`, a `SerializedTaskPump`) runs `SceneSet.cpp:Coordinator::RunSceneWork` → `ApplyEvents.cpp:CollectEvents/ApplyEvents` → walk → joins → `CommitSet` → `PublishScene`. The render thread drains no events. |
| DCLF setting toggles | `Core/Settings/Toggles.cpp:Toggles::BeginFrame`, called from `Core/Render/FrameDriver.cpp:FrameDriver::BeginFrame` | The toggle set travels in `FrameInputs::toggles` to the scene pass. It is stamped into `ScenePublication::toggles`. Frames keep the installed publication's toggles (`Toggles::Adopt` in `FrameView::HandOverAtFrameStart`) until a publication made under the new set is installed. A toggle never takes effect mid-frame. |
| Structural toggles and conditions (cull mode, CPU/GPU pipeline, interior) | `Core/Render/Epochs/GraphConditions.cpp:RefreshGraphConditions` at the build point | `RenderGraph::Builds::Request` for every DCLF segment. |
| Cells loading/unloading | Just attach/detach batches. Load screens are events too (`Scene::Channels::loadingSeen`, `FrameView::NoteLoadingScreen`). | Same scene lane. |
| Render-thread-owned changes: new main resources, new view targets, shadow format, more shadow slots | `Resources.cpp` (`AddExtension`, `PostResourcesAhead`, `Impl::ServeRevisionRequests` → `AddShadowViewSlots`) | `GraphHost::AddExtension` rebuilds the graph. `PostRevisionInputs` tells the builder. |

Note one thing about toggles. `Toggles::BeginFrame` runs at the frame start. It reads a requested set; it is not a per-stage poll.
The requested set is the event. The frame keeps the old set until a publication under the new one is ready.

### 1.2 Events → resource growth

**Who decides capacity.** The snapshot builder, on DCLF's pool, in `DCLF/Core/Render/Revision/Producer.cpp:IndirectDraws::Impl::MakeRevision`.
- It reads the newest publication's tables and the posted `RevisionInputs`.
- Each participant states what it needs: `Requirements.cpp` and `Producer.cpp` (`RequireRevisionSet`, `RequireSceneTables`,
  `RequireMain`, `RequireIndexPool`, `ReserveMainLatch`, `ReserveShadowLatch`, `RequireShadowSequences`, `RequireView`).
- Growth policies come from `OMS/GrowthPolicy` (`Doubling`, `Headroom(pct)`). Shrinking comes from `OMS/ElasticCapacity.h` (`ShrinkPolicy`).
- `RevisionReuse` skips the whole make when nothing it reads moved.

**Seal or grow.** `OMS/GrowableBufferSet` (wrapped by `DCLF/Core/Render/Resources/SceneBufferSet`):
- `GrowableBufferSet::Seal` names the newest ready versions when every requirement is met. It returns the identical `NamedVersions`
  when nothing changed, so recordings stand.
- Otherwise it posts **one growth batch** for every unmet buffer: `SceneGraph::PostGrowth` → `OMS/Async/VersionGrowthArtifact.h:PostVersionGrowth`.
  That is an artifact on DCLF's `StateGraph`.
- On the preparation pool the producer makes the version (`OMS/VersionedBufferGrowth.h:GrowVersions` → ORG `VersionedBuffer::Make`),
  leases a backing from a `BackingPool`, and queues zero/fresh/capture fills on the uploader (`OMS/VersionFill.h:ZeroFill`).
- When the copies complete the version is ready. The builder wakes and seals again.
- **The frame never waits for a growth.** The old snapshot keeps drawing.

**Capacity also gates the scene.** `Snapshot.cpp:Impl::PostFit` gives the coordinator the adopted set's capacities. An object that does
not fit yet is published as pending and withheld until a set that fits is adopted. So a consumer never sees data that its adopted
resources cannot hold. This is the key consistency rule: **data and capacity travel together**.

**Payload parts** (`DCLF/Core/Render/Payload/PayloadVersions`, `PayloadPools::Post`) are a second versioned chain. Version N+1 is
GPU-copied from N plus a journal, through `OMS/Async/VersionedBufferProducer.h:RegisterVersionedBufferProducer`.

**Lifetimes.** One `RetirementClock` (`DCLF/Core/Runtime/FrameRetirement.h`) advances from the host's completed-frame callback. A
backing is reused only after the clock passes its `BackingLease::Bind` point.

**The stages of a version:**

| Step | Where |
|---|---|
| Allocated (pending, not current) | the growth producer, a pooled lease |
| Named | the next `Seal` (a new `NamedVersions` serial), stored as the revision's versions fragment (`Assemble.cpp:AssembleRevision`) |
| Published | when the snapshot carrying it is posted (`Snapshot.cpp:Impl::PostSnapshot`) |
| Made current | frame-start adoption: `NamedVersions::AdoptAll` (ORG `VersionedBuffer::Adopt`) and `GraphHost::NoteNewVersions` |
| Bound for retirement | after the frame's last submission: `IndirectDraws::BindAdopted` |

### 1.3 Cascade into async builds and recordings

**What triggers a build:** `RefreshGraphConditions`, `GraphHost::AddExtension/RemoveExtension`, shader reload
(`RenderGraphRuntime` `RequestRebuild`), `RequestRebuildFor`. The host runs with `SetExplicitBuilds(true)`, so a request only marks the
epoch.

**The build point.** `DCLF/.../Revision/Coverage.cpp:IndirectDraws::BuildPoint` runs first in `FrameDriver::BeginFrame` (guest) or at
`Renderer::Begin` (native, `DCLF::BuildPointAtFrameStart`, called from `src/RenderGraph/Frame/FrameDriver.cpp:OnBegin`). It runs
`RegisterGraphBuilds`, `RefreshGraphConditions`, `BuildIfAsked` (→ `RenderGraphRuntime::BuildIfRequested` → `BuildManager::Poll`).

**Builds off the render thread.** `src/RenderGraph/BuildManager.h:ExternalBuilder` is registered in
`DCLF/.../Epochs/GraphBuilds.cpp:Impl::RegisterGraphBuilds`. A build runs on DCLF's "graph builds" lane with an **immutable**
`Draws::BuildState` (conditions, resources, shadow slots, pass states, a serial). Extensions read only that scoped state, never live
state.

**Attach.** DCLF epochs are `BuildManager::Desc::external`. Their graphs attach only at the frame's end (`Poll("frame end", Attach::All)`)
or at DCLF's own frame-start wait (`RenderGraphRuntime::AttachDclfBuilds`). Then `SettleBuildState` publishes the attached build state,
re-posts revision inputs and wakes the builder.

**What makes a recording stale:** its `buildGeneration` (checked in `Assemble.cpp:RecordingCompleted` and
`GraphHost::EpochRecordingCurrent`), its versions fragment, its shape fragment, and per-pass revision hashes at `SubmitEpoch`.
`MakeRevision` returns early while the build state is newer than the attached one, so it never records against old graphs.

**Re-record off the render thread.** `Assemble.cpp:AssembleRevision` uses `OMS/Async/RevisionAssembly` (`RevisionAssembler`:
`Begin`, `Set(slot, fragment)`, `Seal`, `Collect`, `TrySelect`, `AcquireActive`). Unchanged fragments are inherited. Changed epochs get
`GraphHost::RequestEpochRecording(epoch, EpochRevisionData{versions, shapes})`, recorded on ORG's host thread. **The recording names
the versions it binds** (`Revision.h` exposes `org::IResourceVersions`), so it can be recorded before those versions are current.

### 1.4 How a frame adopts a revision at its start

Order in `Core/Render/FrameDriver.cpp:FrameDriver::BeginFrame`: `BuildPoint` → `Toggles::BeginFrame` → `FrameView::BeginFrame` →
`IndirectDraws::AdoptCurrentSnapshot` → `FrameView::HandOverAtFrameStart` → `DecideCoverage` → claims → frame values → payload.

The snapshot (`Snapshot.cpp`, `Impl::SceneSnapshot`) holds the publication (tables, claims, toggles), the draws, a
`RevisionAssembler::Lease` (versions + a shape and recordings per epoch), the payload versions, and the generations it was built for. It
is posted to a `LatestSlot` (`Core/Runtime/LatestSlot.h`): **latest wins**, older unadopted ones are dropped.

`IndirectDraws::AdoptSnapshot`:
- `Impl::StaleKindOf` rejects a snapshot made for another build, targets generation, shadow format or main generation. The frame
  keeps the old one.
- Otherwise the snapshot is adopted **whole**: `NamedVersions::AdoptAll`, the active revision, the draws, `PayloadPools::Install`.

**What is atomic:** the publication and its toggles, the revision (versions, shapes, recordings of one build), the draws for exactly
that sealed set, and the payload. `RevisionAssembler`'s closure rule means a recording and the versions and shape it was recorded
against are never split. All DCLF epochs attach together (`Attach::All`).

**While a rebuild is in flight:** frames keep the attached graphs and the adopted snapshot. The builder makes nothing for the newer
build state.

**One weakness.** After a frame-end attach, the next frame's `AdoptCurrentSnapshot(true)` finds the adopted snapshot `kStaleBuild` and
**waits** (spins serving builds and recordings, `stl::report_and_fail` after 5 s). This is the one designed stall. DCLF does not use
ORG's staged graphs (`GraphHost::StageEpochGraph` / `AttachStagedEpochGraph`), which CS stages use and which would remove this wait.
This breaks "the render thread never waits" today.

### 1.5 Where DCLF publishes mid-frame on purpose, and why that is safe

- **FrameValues** (`DCLF/Core/Gpu/FrameValues.cpp`): one late-bound buffer per row set, `org::VersionedBuffer::CreateLateBound`.
  `FrameValues::Kick` leases the frame's backings and runs `PublishLate`. The producer fills them during the frame and signals the
  host's frame-wait timeline (`GraphHost::SetFrameWaitValue`).
- **Light contexts and portal programs** (`Features/LocalLights/LightContextResources.cpp`, `Features/Portals/PortalPrograms.cpp`):
  late-bound stand-ins, each publication `PublishLate`d.
- **Latches**: commits write ORG `LatchBlock` regions in `beforeSubmit` or `UseHeldRun`.

Why it is allowed (ORG `docs/persistent-epochs.md`, "Late binding"; `Resources/Buffers/VersionedBuffer.h`):
- **No recording binds the data.** Shaders read it by a bindless index or address that is written into latch or frame data.
  Declarations resolve to a fixed stand-in. A new version is not a binding edit, so kept recordings stay valid.
- **The upload planner orders it.** Each batch that declares the stand-in counts as a user of the live backing
  (`detail::BindLateBacking`).
- **Lifetime** is by the `RetirementClock`. Producer waits only target sealed GPU points.

So the rule is not "nothing changes mid-frame". The rule is: **anything a recording binds changes only by adoption at a boundary;
late-bound data may change any time.** That rule is what the new system should enforce for everything.

### 1.6 Reusable versus DCLF-specific

| Layer | Pieces | Reusable? |
|---|---|---|
| ORG | `VersionedResource`, `VersionedBuffer` (`Make`, `Adopt`, `Publish`, `CreateLateBound`, `PublishLate`), `VersionedTexture` (`Make`, `MakeImage`, `Adopt`), `IResourceVersions` (`Find`, `FindTexture`), `GraphHost` builds (`BuildEpochGraph`, `AttachEpochGraph`, `StageEpochGraph`, `AttachStagedEpochGraph`, `BuildGeneration`), revision-driven epochs (`RequestEpochRecording` with host data and a generation, `EpochRecordingCurrent`, `EpochRecordingVersionsCurrent`), `LatchBlock` | Yes, all generic |
| OMS | `GrowableBufferSet`, `NamedVersions`, `GrowthPolicy`, `ElasticCapacity`, `BackingPool`, `RetirementClock`, `VersionFill`, `Async/StateGraph` + `ArtifactIdentity` (`Exact`, `Latest`, `ReadyGateAtLeast`, ...), `Async/RevisionAssembly`, `Async/SerializedTaskPump`, `Async/PublicationExchange` | Yes, generic. `NamedVersions` covers buffers only: it does not implement `FindTexture` (`OMS/GrowableBufferSet.h`). |
| CS generic | `BuildManager` (`ExternalBuilder`, `staged`, `SwitchStaged`, `Builds::Defer`/`DeferStaged`), `RenderGraphRuntime::BuildIfRequested` | Yes |
| DCLF policy | `GraphBuilds.cpp` (`BuildState`, `SettleBuildState`), `GraphConditions.cpp`, `Coverage.cpp` (`BuildPoint`, `RevisionInputs`), `Producer.cpp`, `Requirements.cpp`, `Assemble.cpp`, `Snapshot.cpp` (snapshot struct, `StaleKindOf`, the frame-start wait), `Toggles`, `LatestSlot`, `PublicationLog` | Pattern reusable, code DCLF-shaped |

The pattern under DCLF is generic: **immutable input state → off-thread build → versions named in a revision → recordings against
named versions → one posted set → whole adoption at the start.** Only the snapshot contents and the staleness kinds are DCLF's.

---

## 2. What the frame/graph side has today

### 2.1 `GraphResources` twins and `RequestSize`

`src/RenderGraph/GraphResources.h` (header comment) and `GraphResources.cpp`:
- A twin is a CS-side texture or buffer with a graph resource and optional D3D11 exports. `Options::lifetime` is `Transient`,
  `History` or `Persistent`. `Options::versioned` makes it keep one `org::VersionedTexture` slot for life.
- **Resize, same shape class, versioned:** the new texture is published as a new version of the slot (`Resource::AdoptSlot`). No build.
- **Any other Resize** (not versioned, aliasable, other mip/format/slice count): `Resource::BumpResizeGeneration`. The frame anchors poll
  `ResizeGeneration()` (below) and rebuild every graph.
- **`Texture2D::RequestSize` / `Texture3D::RequestSize`** → `Resource::RequestExtent`: records the wanted size. The first request queues
  `ApplySizeRequests` through `Frame::Driver::AtNextBegin`. A request for the current size cancels. At Begin each live twin runs
  `Resize`. A failure calls `stl::report_and_fail`.
- **The texture is allocated on the render thread at Begin** (`ApplySizeRequests` → `Resize` → `Create`, inferred from the call
  chain: `RequestExtent`'s lambda calls `Resize`, which re-creates the D3D11 export too). So a resize costs an allocation at the start of
  the frame on the render thread.

### 2.2 `Frame::Driver::AtNextBegin`

`src/RenderGraph/Frame/FrameDriver.cpp:AtNextBegin` pushes a closure. `OnBegin` runs every closure first, before the tap check, the
kind decision (`DecideKind`), DCLF's native build point, `PostChain::OnBegin`, `Recipes::Check` and `FrameAnchors::BeginFrame`. It is a
generic "do this at the next boundary" hook. Users today: `RequestSize`, Terrain Shadows' height-map `Switch`
(`ORGTerrainShadows::SetHeightmap`), ImGui's atlas pages (`ImGuiBasicRHI.cpp:SetPage` → `PublishPages`).

### 2.3 `Frame::Resources`

`src/RenderGraph/Frame/FrameResources.cpp` (`Texture2D`) is the kept-texture cache for ImageSpace's chain (TAA history, bloom, adapt).
A same-class size change calls `RequestSize` and returns `fresh=true`. Otherwise it makes a new twin. `fresh` adds one-shot clear passes
(`taa.history{h}.clear`, ...). So history reset is tied to "a new texture was returned at registration".

### 2.4 `FrameAnchors`: registration, builds, staged switch

`src/RenderGraph/FrameAnchors.cpp`:
- **Register / Refresh / Unregister / Rearm** each call `Publish` and `Rebuild(mask)` → `Builds::Request`. These are called from feature
  prepasses during the frame. That is normal today (section 3).
- **`Service`** runs at every anchor `Run` (inside the frame). It polls `GraphResources::Resource::ResizeGeneration()`. On a change it
  calls `PublishedResources::Refresh`, `Publish`, and `StructuralRebuild` on every anchor. **This is an in-frame poll.**
- **`BeginFrame`** (called from `FrameDriver::OnBegin`) runs `Schedule` per declared segment (stage), then `UpdateKey` per stage.
- **`Schedule`** is the per-stage switch:
  - A schedule of an older build is dropped (`EpochRecordingCurrent`).
  - If the slot's recording binds a replaced version (`EpochRecordingVersionsCurrent` false), the stage's version generation moves
    and a new recording is asked for. **Until it is ready the stage keeps the old recording, which holds the old version.** The
    stage lags one or two frames; it is not blanked.
  - A staged build (`host.StagedGeneration(epoch)`) is described and recorded for every frame slot. When all four recordings are done,
    `Builds::SwitchStaged(epoch)` attaches it and every slot's schedule switches at once. A starvation rule (`kAskedAgainLimit`) reports
    a tap error if requests keep replacing its recordings.
- **`UpdateKey`** (the absent-pass key) asks for a build when a frame wants passes the build left out.
- **Engine imports**: `Import` → `JoinSlot` (a new D3D11 texture becomes a new version of the import's slot; a new shape class is a
  structural rebuild) and `EnsureViews` (a new version with more views). These publish **when a pass first imports**, i.e. mid-frame.

**Important (inferred from `Schedule`): switching is per stage.** Each stage switches when its own recordings are ready. Nothing ties two
stages together. A resource version adopted at Begin is bound by every stage whose recording is ready for it, while a lagging stage still
binds the old version. If a writer and a reader of one texture sit in different stages, one frame can have the writer write version
N+1 while the reader reads version N (the last contents written into N). Today this shows as a frame of stale or old-size data, not a
crash. The design doc says so too: "the recipe is not replaced as a whole at a frame boundary (stages are rebuilt one by one as clients
change)" (`docs/development/org-native-frame-graph.md`, the BasicRenderer comparison).

### 2.5 `BuildManager`

`src/RenderGraph/BuildManager.h`:
- Every request goes through `RenderGraph::Builds` (`Request`, `RequestAll`, `RequestFor`, `RequestAllSynchronous`, `Change`). The host
  only marks the epoch.
- `Poll` at frame boundaries attaches finished builds and hands requested epochs to the build thread.
- `Desc::staged`: every CS stage's epoch. A finished build is staged (`GraphHost::StageEpochGraph`), recorded by the host thread, and
  switched in by its owner (`SwitchStaged`).
- `Builds::Defer(attached, failed)` runs work at the graph's attach. `Builds::DeferStaged` runs work when it is staged.
- `Desc::external`: DCLF's epochs, attached only at the frame end or DCLF's wait.
- `RequestAllSynchronous` (shader reload's pipeline swap) builds on the owner thread with the host thread held.

### 2.6 ORG `GraphHost`

`ORG/include/OpenRenderGraph/GraphHost.h`:
- **Builds and generations.** `BuildEpochGraph` (any thread), `AttachEpochGraph`, `BuildGeneration(epoch)`.
- **Staged graphs.** `StageEpochGraph`: the host thread records the staged graph for requests that name its generation. The attached
  graph keeps submitting **with its aliased bindings frozen** (`RenderGraph::FreezeAliasedBindings`) and takes no new recording.
  `AttachStagedEpochGraph(epoch)` swaps graph and recordings in one step. **One epoch per call.** There is no multi-epoch attach.
- **Recordings.** `RequestEpochRecording(epoch, hostData, done, copy, generation)`. `hostData` can expose `org::IResourceVersions`.
  Then the preparation binds the **named** versions (`IResourceVersions::Find` for buffers, `FindTexture` for textures), not the current
  ones (`docs/persistent-epochs.md`, "Versioned resources": "A preparation for host data binds the version the data names").
- **Staleness.** `EpochRecordingCurrent` (build generation), `EpochRecordingVersionsCurrent` / `EpochRecordingStaleVersion` (named
  slot), `NoteNewVersions` (buffers).
- **Frame alias arena.** `SetFramePositions` gives each epoch a place in the frame and makes a `FrameAliasArena`. `AddFrameTransients`
  places textures that several epochs use in one frame. Their span is **derived** from each graph's alias plan, never declared. A span
  only grows, in place; a placement never moves. A growth into bytes another range holds fails that graph's plan. **Textures made again
  under the same label (a resize) start with the span the last ones reached.**
- **Shared alias arena** (`Desc::sharedAliasArena`) and `SetPrivateAliasEpochs` for the epochs' own transients.

### 2.7 `PublishedResources`

`src/RenderGraph/PublishedResources.h`: a twin published under an id is registered with every epoch's graph (anchors' and DCLF's). A
versioned twin is published as its slot, so a resize is a version readers follow without a build. A new resource under an existing id
is `RequestStructuralRebuild` of **every** graph. `Refresh` re-publishes resized twins when the resize generation moves.

### 2.8 Where the current design still breaks the target rules

| Rule | Where it is broken today |
|---|---|
| No per-frame "do I need to resize" in the frame | Feature prepasses compute sizes and call `RequestSize`, `Refresh`, `Register` every frame (section 3). `FrameAnchors::Service` polls the resize generation at each anchor run. `ImageSpace.cpp:CheckImports` and `ChainSteps.inl:CheckChainSteps` poll once a frame. |
| No queueing from inside the frame | `RequestSize`, per-feature `AtNextBegin`, LLF's pending cluster shape (`ORGLightCulling::BeginFrame`). |
| Allocations off the render thread | `ApplySizeRequests` allocates at Begin on the render thread. Upscaling's method change calls `DrainCommandRing` and re-creates textures mid-frame (`Upscaling.cpp:CheckResources`). LLF local shadows `Release` + `Setup` mid-frame. |
| A frame never mixes a new texture with an old recording | Per-stage switching (2.4). Engine imports `JoinSlot`/`EnsureViews` publish mid-frame. |
| The render thread never waits | DCLF's `kStaleBuild` wait after a frame-end attach (1.4). |
| Versions published only at the boundary | Engine imports; guest anchors' own epochs (HDROutput, FrameGeneration) may take a build mid-frame (`FrameDriver.cpp:OnBegin` comment). |

---

## 3. Inventory of variables

"Polled" means checked every frame inside the frame. "Event" means a callback at the source. "Setup" means read once and never followed.

### 3.1 Frame-wide variables

| Variable | Read where | When | Action | Invalidates |
|---|---|---|---|---|
| Render size / dynamic resolution ratios (`Util::ConvertToDynamic`, `graphicsState.dynamicResolution*Ratio`; written each frame by `Upscaling.cpp:ConfigureUpscaling`) | `Frame/PostChain.cpp:Read` (`post.dynres`, `post.taa`, `post.upscaler`) | Polled at `OnBegin` | Into the frame description and the build key | ImageSpace chain rows, recording, key build |
| same | `ChainSteps.inl:CheckChainSteps` (first ImageSpace call, `ImageSpace.cpp:BeginFrame`) | Polled in-frame | `RegisterChainSteps`: full re-register of 3 clients | Build (viewports baked in) |
| same | `ORGExponentialHeightFog.cpp:Prepass/FollowSizes` via `ExponentialHeightFog.cpp:ComputeGridSizes` | Polled | `RequestSize` on 4 Texture3D + 2 Texture2D (versioned) | Texture sizes, constants (from `VolumeTextures::Current()`), `ResetHistories` |
| same | `ORGPostProcessing.cpp:BeforeTonemap` (`shape.dynamicWidth/Height` in `key`) | Polled | `Impl::Rebuild` (new transient twins, re-Register) | Build |
| same | `ORGSunRays.cpp:PrepareFrame` (half sizes in `Shape`) | Polled | `Build`: new transient twins, Register | Build |
| same | `LightLimitFix.cpp:UpdateClusterParameters` → `ORGLightCulling.cpp:Execute` | Polled | New `Shape.clusterSize` into `pending`, applied in `ORGLightCulling::BeginFrame` | Pass revision, recording (buffers sized once at Setup) |
| same | `ORGScreenSpaceShadows.cpp:Prepass` (`GetScaledSampleCount`) | Polled | New program → `Refresh` (unseen sizes built synchronously at first use) | Recording/build |
| same | `ORGScreenSpaceGI.cpp:Commit` (`GetDispatchSizes`) | Per run | Clamped to `Desc()` | Nothing (latch) |
| same | `ORGVolumetricLighting.cpp:BlurArea` | Per frame | Latch only | Nothing |
| Upscaler method | `Upscaling.cpp:CheckResources` (static `previousUpscaleMode`) | Polled | `DrainCommandRing`, destroy/create upscaled and HUD-less textures → `ORGUpscaling::CreateTexture`, `UpscaleChanged`/`HudlessChanged` re-register mid-frame, `FrameGen::Controller::Reconcile` | New non-versioned twins, builds |
| Upscaler presence | `ORGUpscaling.cpp:RegisterUpscale` (`declared = post.upscaler`) | At Begin (declared predicate) | Pass presence | Description/recording |
| Upscaler quality | → resolution scale → the render-size rows | Polled | as above | as above |
| Display / window size | `Engine/Targets/EngineTargets` `BeginSet/EndSet` | **Event** (engine's `WindowSizeChanged`) | New engine target set, `BumpResizeGeneration` once | Structural rebuild at the next `Service` |
| Engine target identity/size | `FrameAnchors.cpp:Import` (every `EngineTarget`/`EngineDepth` call from feature prepasses) | Polled in-frame | `JoinSlot` (version), `Rebuild(~0, "import resized")`, `StructuralRebuild` for a new shape class, `EnsureViews` | Versions mid-frame, builds |
| Engine targets for ImageSpace | `ImageSpace.cpp:CheckImports`, `CheckChainSteps` (kMAIN, final target size/object, luminance levels) | Polled once a frame | `ReregisterSteps` | Build |
| Native back buffer | `NativeDevice.cpp:Resize` (engine's `ResizeBuffers`) | Event | Drops back buffer, `RequestStructuralRebuild` | Every build |
| Guest back buffer | `OwnedFrame.cpp:Present` (`needsResize`) | Polled per present | `Resize`, `ReadImages` | Outside the graph |
| HDR on/off | `HDRDisplay.cpp:BeginRenderFrame` | Polled | `hdrEnabledForFrame`, `UpdateHDRData`, colour space | No texture change |
| HDR composite inputs | `ORGHDRDisplay.cpp:Compose` (scene source, back buffer pointer/format/size/count, program, clear target) | Polled per present | Re-registers HDROutput client mid-frame (own epoch) | Build |
| HDR final target | `ChainSteps.inl:FinalName/CheckChainSteps` | Polled | Chain re-registers | Build |
| HDR UI texture | `BinkGraph.cpp` (Refresh site), `Menu/ImGuiRenderer.cpp:UpdateTarget` (`OMGetRenderTargets`) | Polled | Re-import + `Refresh` | Build |
| Swap chain format | `HDRDisplay.cpp:SetupResources` | Setup | Exported twins (`ORGHDRDisplay::MakeTexture`) | — |
| Guest colour space | `OwnedFrame.cpp:ApplyColorSpace` | At present | — | Outside the graph |
| Frame kind (world, frozen world, menu, loading, Bink) | `FrameDriver.cpp:DecideKind` | At Begin, from events | `Recipes::KindHas` selects stages; kind switch lets unused passes leave (key build) | Recipe per frame, keys |
| ImGui atlas pages | `ImGuiBasicRHI.cpp:SetPage` | Event (texture create/destroy) | `AtNextBegin(PublishPages)` (versioned slot) | UI stage recording |
| Font reload | `ImGuiBasicRHI.cpp:InvalidateTextures` | Event | same | same |
| Shader reload | `RenderGraphRuntime::RequestShaderReload` → `ServicePipelineReload` (at present) | Event | `Builds::RequestAllSynchronous` | Every build (synchronous) |
| Per-feature shader recompile | SSGI `recompileFlag`, SSS/DynCubemaps `programsDirty`, HDR `programDirty`, PostProcessing, Upscaling, TerrainBlending, Effects11 `Invalidate` | Flag set by event, checked polled | Programs cleared, Refresh/Rebuild, history reset (SSGI) | Builds |
| Client presence/activity | `ClientDesc` `declared()`, `active()`, `rows`, `instances` | `declared` at Begin; `active` at the tap | Absent-pass key (`FrameAnchors.cpp:UpdateKey`) | Key build |

### 3.2 Feature toggles at runtime

`loaded` is fixed at startup (`ForEachLoadedFeature`). Runtime "on/off" is each feature's own setting.

| Feature | Read where | When | Action | Invalidates |
|---|---|---|---|---|
| SSGI enabled | `ORGScreenSpaceGI.cpp:Prepass` (`Config::Current().enabled`) | Polled | Off: unregister both clients, withdraw outputs. On: build + register | Builds |
| EHF enabled / volumetric / extinction | `ExponentialHeightFog::Prepass` | Polled | Off: `Release`. On: `MakeTextures` with new twins, **republished under the same ids → structural rebuild of every graph** | Every build |
| Terrain Blending | `Main_RenderDepth` (`settings.Enabled`) | Polled | Gates `BeforeDepth`; SSS shadows `CurrentDepth` follows, `Refresh` | Build |
| PostProcessing effect list | `pp.UpdatePipeline()` / `ChainOrder()` | Polled | Effect list in `key` → `Rebuild` | Build |
| Effects11 | `ORGFxRuntime::Prepare` (`Runs(fx)`, tonemap owner, preset in `ShapeKey`) | Polled | Planner + Register, or `Inactive` unregister | Build |
| IBL | `ORGIBL::Prepass(useEnvironment)`, `client.declared = active` | Polled | `Refresh` | Build/description |
| Tonemap owner | `State::GetTonemapOwner` in both prepares, `post.effects11` in the description | Polled | — | Build/description |

### 3.3 Per-feature settings that change sizes or pass sets

| Feature / variable | Read where | When | Action | History reset |
|---|---|---|---|---|
| EHF grid (`volumetricGrid*`) | `ORGExponentialHeightFog.cpp:Prepass` | Polled | Extent → `RequestSize`. `Shape` (groupReduce, farEnabled, temporal, inputs) → `Build` | `ResetHistories` on extent change |
| SSGI resolution mode, GI, specular, temporal, blur | `ORGScreenSpaceGI.cpp:Prepass` (new `Config`) | Polled | `Build(config, prev)` keeps same-shape textures, else new twins; `RegisterReset` + `RegisterChain`; `PublishOutputs` of a new texture → structural rebuild | Reset client's one-shot clears |
| SSGI size | same | Polled | `RequestSize` to kMAIN; once `Desc()` changes, re-register chain + `Rearm(kResetClientId)` | Yes (`Rearm`) |
| SSS shadows mask size / sample count | `ShadowMaskSize`, sample count | Polled | `RequestSize` (versioned, exported) / `Refresh` | None needed |
| Terrain Blending depths | `BeforeDepth` | Polled | `RequestSize` on 3 versioned depths; after the switch `RegisterClient` again for group counts | — |
| Skylighting probes | `Skylighting.h` `{256,256,128}` | Constant | Never resized. `ResolveInputs` → `Refresh` (polled from `Update`). `ResetSkylighting` → `QueueReset` → `Rearm` (event) | On reset event |
| Terrain Shadows height map | `TerrainShadows::LoadHeightmap` → `ORGTerrainShadows::SetHeightmap` | Event (worldspace change) | New persistent twin + staging, `AtNextBegin` → `Switch`: `shadowHeights.Resize` + new client (one-shot upload) | n/a |
| Dynamic Cubemaps resolution | `ORGDynamicCubemaps` `Setup` | Setup | Never followed. `AfterUpdate` (polled) handles `programsDirty`, re-imports, adds bc6h copy pass, `Refresh` | No |
| Shadow map resolution (volumetric shadows) | `VolumetricShadows` `UpdateImports` (EarlyPrepass) | Polled | `Refresh` (copy textures fixed at `kCopySize`) | — |
| LLF local shadows (`LocalShadowResolution`, `Slots`, engine shadow map size/format) | `EnsureLocalShadowResources` (from `CopyLocalShadowMaps`) | Polled | `ORGLocalShadows::Release` + `Setup` **mid-frame**: new exported twins, Register, publish replaced → structural rebuild | — |
| LLF cluster count | `LightLimitFix::SetupResources` | Setup | Fixed | — |
| Volumetric Lighting volume size (interior/exterior) | `SettingsSizes` at registration; `EndRender` sees the engine size | Polled | `RegisterAgain` with new transient Texture3D twins; frame skipped until `Scheduled` | — |
| Post effects (CODBloom mips, DoF, PhysicalGlare FFT size, LensFlare, LUT, ColorGrading, depth/motion import keys) | per-effect `Update` into `key` | Polled | `Rebuild`; kept textures recreated inside `Build` when their shape changes (`ORGMotionBlur` `horizontal`) | MotionBlur none; HistogramAutoExposure `clearPending` |
| ImageSpace kept textures (TAA history, bloom, adapt) | `RegisterChainSteps` → `MakeOwned` → `Frame::Resources::Texture2D` | Polled (via CheckChainSteps) | `RequestSize` (`fresh=true`) or new twin | One-shot clear passes |
| Cloud Shadows, Grass Collision, Glints noise | Setup | Setup | — | — |
| DCLF query readback chunks, targets (`SkyDraws`, `DecalOcclusionDraws`, `BloodSplatterDraws`, `ShadowMaskDraws`) | on add/change | Event-ish (at add) | `Refresh` | — |

### 3.4 What the inventory shows

1. **Almost everything is polled in-frame.** Only the display size (engine `WindowSizeChanged`), the native back buffer, Terrain Shadows'
   height map, ImGui's atlas, shader reload and Skylighting's reset are events.
2. **Five sites use `RequestSize`:** EHF, SSGI, SSS shadows, Terrain Blending, `Frame::Resources`. **Two use their own `AtNextBegin`:**
   Terrain Shadows, ImGui. **LLF clusters** apply a pending shape in their own `BeginFrame`.
3. **Most changes re-create non-versioned twins mid-frame** and rely on the next build being switched in at a boundary: Upscaling,
   Sun Rays, post effects, Volumetric Lighting, LLF local shadows, SSGI configs, EHF re-enable.
4. **Three paths rebuild every graph** because a new object is published under an existing id: EHF re-enable, LLF local shadows, SSGI
   outputs on config change.
5. **Several features are sized only at setup** and are correct only because their inputs never change in a session: SSS working
   textures, Dynamic Cubemaps, Skylighting probes, Volumetric Shadows copies, Cloud Shadows, LLF buffers.
6. **The variables are few.** Most rows reduce to: render size (dynamic ratios), display size, upscaler method, HDR on/off and output
   target, engine shadow map size/format, the engine reflection cubemap size, and a handful of per-feature settings. A small fixed
   registry would cover them.
7. **Settings have more than one writer (inferred from `State.cpp`, `FeatureListRenderer.cpp`, `CSEditor/SceneManager`).** The menu
   widgets write plain struct fields; there is no per-field change event. `FeatureListRenderer.cpp` detects "this feature changed" around
   `feat->DrawSettings()` with ImGui's `ActiveIdHasBeenEditedThisFrame` and `Util::InteractionEndedThisFrame`. Other writers: presets,
   `LoadSettings`, undo (`FeatureUndo.cpp`), the scene settings manager's layers (`SceneSettingsManager`), and cs-test's `setting` step.
   A central event source must cover all of them.

---

## 4. Design options

### 4.0 What every option must provide

These hold for all three options. They follow from sections 1 and 2.

**R1. Two kinds of data, two rules.**
- *Bound* data: anything a recording binds (versioned textures and buffers declared by passes, graph structure, programs, pass
  constants that are prepared into a ticket). It changes **only** by adopting a prepared set at Begin.
- *Late-bound* data: read through bindless indices or latches, not bound by a recording (DCLF frame values, light contexts, portal
  programs, per-run latches). It may change any time, as today.

**R2. Record against pending versions.** The cascade only works without lag if a stage can be recorded for a texture version that is
not current yet. ORG supports this: host data exposes `org::IResourceVersions::FindTexture` and the preparation binds the named version
(`ORG/docs/persistent-epochs.md`, "Versioned resources"; `Resources/VersionedTexture.h`). DCLF does this for buffers
(`OMS/GrowableBufferSet.h:NamedVersions`). CS stages do **not**: `FrameAnchors.cpp:SegmentHostData::TryGet` returns only itself. This is
the one missing primitive that makes "adopt atomically" possible. Without it a stage can only be recorded after the new version is
current, which is exactly the one-stage-at-a-time lag of section 2.4.

**R3. One adoption for several epochs.** `GraphHost::AttachStagedEpochGraph` takes one epoch. A set that changes the fog volumes touches
pre-main (writer), DCLF's colour epochs and post (readers, inferred from `PublishedResources` and the "declared view mismatch" log in
`docs/development/parity/status.md`). Adoption must attach every member epoch's staged graph, switch every member stage's schedule and
adopt every member version in one step at Begin, or not at all.

**R4. The closure is derived, not declared by hand.** Given the changed slots, the member epochs are those whose graphs declare them.
ORG knows this already (`EpochRecordingStaleVersion` finds the slot that makes a recording stale). A function such as
"`GraphHost::EpochsDeclaring(slot)`" (new) gives the closure.

**R5. Constants follow the set.** Sizes in constants (EHF slice counts, SSGI dispatch, LLF cluster grid, viewports) must be computed
from the **adopted set**, not from live settings. The frame reads `set.Extent(texture)` or the adopted variable values.

**R6. History is reset by the birth of a version.** A new version of a `History` resource is born cleared (an upload-queue zero fill, like
DCLF's `VersionFill::ZeroFill`, before the version is ready). The set also carries a per-resource "first frame" flag that the feature's
temporal pass reads from its constants. This replaces `Rearm`, `ResetHistories`, `fresh` clears and `clearPending`.

**R7. Latest wins, frames keep the old set.** Sets go into a `LatestSlot`. While one is being prepared, every frame runs the last adopted
set, which is consistent. A newer event supersedes a set in preparation. A starvation rule (as `kAskedAgainLimit` in `Schedule`)
catches a set that never completes.

**R8. Failure is fatal.** A preparation that fails (allocation, build, recording) calls `stl::report_and_fail` with the variable, the
resource and the epoch named. No fallback set, no engine fall-through.

**R9. Alias arena.** Staged graphs already freeze the attached graph's aliased bindings while staged (`StageEpochGraph`). A resized
frame transient is "made again under the same label" and inherits its span (`GraphHost::AddFrameTransients`). The new textures need
placements while the old set's placements are still in use by frames in flight. **(inferred)** A frame-arena range that only grows in
place may not have room for a larger texture next to the live one; that case fails the plan, which by R8 is fatal. Two answers: (a) the
set reserves a second range for the members of a resize and retires the old range when the old set retires; (b) frame transients are
sized to the maximum render size (display size), and a dynamic-resolution change is a viewport change, not a resize. (b) removes the
most frequent resize entirely and is what most engines do for dynamic resolution.

**R10. Engine-driven changes.** Two cases:
- *Announced*: the engine re-creates its targets through a call CS hooks (`Renderer::WindowSizeChanged` → `EngineTargets::BeginSet/EndSet`;
  native `NativeDevice.cpp:Resize`). That is an event like any other. The set is prepared off-thread and adopted at a Begin.
  In native mode CS creates those textures itself, so it can make the new ones as pending versions **(inferred from
  `EngineTargets.h`: "makes them on ORG directly")**.
- *Discovered*: a guest pass imports a D3D11 texture it has not seen (`FrameAnchors.cpp:Import` → `JoinSlot`). The frame cannot have
  prepared for it. Options: keep today's mid-frame publish as the single documented exception; or make it fatal and find each source
  until none is left (the ORG "remove the trigger" approach, like the zero fall-through gate); or let that frame skip the affected
  stages. See question 3.

### 4.1 Option (a): a variables-and-dependents registry in CS, on today's ORG

**Shape.** A CS module, `src/RenderGraph/Frame/Inputs.{h,cpp}` and `Frame/PreparedSet.{h,cpp}` (names are suggestions).

```cpp
// Variables: one writer each, written from events, read through the adopted set.
namespace Frame::Inputs {
    extern Variable<Extent2D>     renderSize;      // written by Upscaling's configure step on a change
    extern Variable<Extent2D>     displaySize;     // EngineTargets::EndSet, NativeDevice::Resize
    extern Variable<UpscaleMethod> upscaler;
    extern Variable<bool>         hdr;
    // Per-feature: a feature registers its size-relevant settings as a variable with a projection.
    template <class T> Variable<T>& Register(std::string_view name, T initial);
}

// A feature declares a resource as a function of variables.
auto fog = Frame::Declare<GraphResources::Texture3D>("ExponentialHeightFog::VBufferA",
    { Inputs::renderSize, fogGrid },                       // depends on
    [](const Inputs::View& v) { return FogVolumeDesc(v.Get(Inputs::renderSize), v.Get(fogGrid)); },
    { .lifetime = Lifetime::History, .versioned = true }); // History: born cleared, first-frame flag

// Pass-set changes are declared the same way: a client's shape is a function of variables.
Frame::DeclareClient(kFogClient, { fogGrid, fogEnabled }, [](const Inputs::View& v) { return MakeFogClient(v); });

// In the frame: read only the adopted set.
const auto& set = Frame::Adopted();
constants.slices = set.Extent(fog)[2];
```

**Cascade.** On a variable write the registry posts a preparation job (a `SerializedTaskPump`, like DCLF's lanes):
1. Recompute the descriptors of the resources that depend on the changed variables.
2. For a same-class change: `VersionedTexture::Make` the new texture (any thread; ORG allows it) and queue its birth fill.
3. For a new shape class or new client shape: request the stage builds (`Builds::Request`) for the epochs involved. They are staged.
4. Derive the member epochs (R4). For each member stage and frame slot, request a recording against the pending versions and the staged
   generation (needs R2: `SegmentHostData` exposes `IResourceVersions`).
5. When every member is ready, post the set to a `LatestSlot`.

**Adoption at Begin.** In `FrameDriver::OnBegin`, where `AtNextBegin` runs today: take the latest set. Adopt every pending version
(`VersionedTexture::Adopt`), attach every staged member epoch (`AttachStagedEpochGraph` per epoch, all-or-nothing on CS's side), switch
every member stage's schedules, install the variable values the set was made for. Then `FrameAnchors::BeginFrame` describes the frame
against the adopted set.

**What moves where.** Everything new is in CS. ORG needs nothing new, except optionally a multi-epoch attach.

**Deletes.** `RequestSize`/`RequestExtent`/`ApplySizeRequests`, the `AtNextBegin` users (Terrain Shadows `Switch`, ImGui `PublishPages`
become variable events or set members), the per-frame size checks in every prepass in section 3, `FrameAnchors::Service`'s
resize-generation poll, `ResizeGeneration`/`BumpResizeGeneration`, `CheckChainSteps`/`CheckImports` polling, LLF's pending cluster
shape, Upscaling's `previousUpscaleMode` and its mid-frame `DrainCommandRing` re-create, `Rearm`-for-reset, `fresh`.

**Pros.** Smallest change to ORG. Fast to start. Features migrate one at a time.

**Cons.** It is app-side bookkeeping (the user's stated preference is graph primitives over that). Two adoption mechanisms stay: this
one and DCLF's snapshot. The closure (R4) and the multi-epoch atomicity (R3) are emulated in CS around a host that does not know about
sets. Per-epoch `AttachStagedEpochGraph` calls can be refused one by one (configuration changed), which leaves a half-adopted set that CS
must detect and treat as fatal.

### 4.2 Option (b): resource descriptors as functions of variables, owned by ORG

**Shape.** ORG gains a host-level "configuration" primitive.

```cpp
// ORG (GraphHost or a new org::Configuration):
auto renderSize = host.DeclareVariable<Extent2D>("renderSize", initial);
auto vbufA = host.DeclareTexture("ExponentialHeightFog::VBufferA",
    org::DependsOn(renderSize, fogGrid),
    [](const org::VariableView& v) { return org::TextureDescription{ ... }; },
    org::TextureTraits{ .history = true });

// Any thread: an event.
host.Set(renderSize, Extent2D{ 1920, 1080 });

// Owner thread, at Begin:
if (auto set = host.TakePreparedConfiguration())   // latest complete set, or null
    host.AdoptConfiguration(*set);                  // versions + staged graphs + recordings, all epochs, atomically
```

**Cascade inside ORG.** The host thread (or a preparation pool the host owns) recomputes descriptors, makes versions, decides which
graphs must be rebuilt (a descriptor change of shape class, or an extension whose shape function depends on the variable), builds and
stages them, records every epoch that declares a changed slot against the pending versions, and publishes a configuration when complete.
Graph extensions can read variables at build time (`org::VariableView` in the build context) so pass sets depend on variables too.

**What moves where.** The cascade, the closure, the atomic adopt, history birth fills and the alias-arena placement of re-made frame
transients move into ORG. CS keeps the variable writers (event sources), the feature declarations and the frame recipe.

**Pros.** One primitive with no app-side list. Matches the user's ORG preferences ("graph primitives over bookkeeping", "async builds
switched in at the next snapshot", "remove the trigger"). The host already owns the facts needed for R3, R4 and R9 (which epochs declare a
slot, the alias plans, staged graphs). BasicRenderer and other ORG hosts get it too.

**Cons.** The biggest ORG change. CS's stage switching (`FrameAnchors::Schedule`: descriptions, frame slots, the absent-pass key,
recordings per description) lives in CS and would have to either move into ORG or call into the configuration. CS's twins (`GraphResources`)
with D3D11 exports would become thin wrappers over ORG descriptors; the D3D11 export re-creation on a new version stays in CS
**(inferred: exports are CS's, `GraphResources.h` header)**. DCLF's revision machinery would still be a second mechanism unless (c) is
also done.

### 4.3 Option (c): one mechanism, unified with DCLF's revisions

**Shape.** The frame has one revision. DCLF's snapshot becomes a part of it. The machinery is OMS's, which is already generic:
`Async/StateGraph` artifacts, `Async/RevisionAssembly` (`RevisionAssembler` with fragments and the closure rule), `NamedVersions`, `LatestSlot`.

```cpp
// A frame revision is a RevisionAssembler over fragments:
//   kInputs     : the variable values (render size, HDR, toggles, per-feature settings)  -- includes DCLF's Toggles
//   kVersions   : NamedVersions for buffers AND textures (FindTexture implemented)
//   kBuilds     : the staged generation per epoch (CS stages and DCLF epochs)
//   kRecordings : per epoch, per frame slot (CS stages: per description; DCLF: per revision)
//   kScene      : DCLF's snapshot (publication, draws, payload)
// Begin: AdoptAll(frameRevision) -- one call, every fragment, or the previous revision stays.

// A feature declares as in (a) or (b); its resources' versions are produced as StateGraph artifacts:
auto vbufA = FrameRevision::DeclareTexture("ExponentialHeightFog::VBufferA", { renderSize, fogGrid }, MakeDesc, { .history = true });
```

**Cascade.** A variable write posts an artifact request. Producers make texture versions (like `PostVersionGrowth` makes buffer versions),
the build lane produces staged graphs, the host records against named versions, and the assembler seals a revision when the closure is
complete. DCLF's scene revisions keep their own producer but seal into the same assembler.

**Deletes**, beyond (a): DCLF's frame-end `Attach::All` for external epochs and the `kStaleBuild` wait in `AdoptCurrentSnapshot` (DCLF
epochs become staged and switch with the rest), the separate `Toggles::BeginFrame`/`Adopt` path (toggles become variables in the
inputs fragment), `GraphConditions` polling at the build point (conditions become variables), `RenderGraphRuntime::AttachDclfBuilds`.

**Pros.** One mechanism, one adoption point, one staleness model. DCLF's proven primitives do the work. Atomic across CS stages and DCLF
epochs, which matters because DCLF epochs read CS-published textures.

**Cons.** The largest migration. DCLF is the most complex and best-tested part; touching its adoption risks regressions there. DCLF's
snapshot cadence (scene changes every frame while moving) is much higher than resize events; one assembler must not make a resize wait
behind a scene revision or the reverse **(inferred risk)**. Needs R2 and R3 in ORG anyway.

### 4.4 Comparison

| | (a) CS registry | (b) ORG owns it | (c) unified with DCLF |
|---|---|---|---|
| Where the cascade lives | CS | ORG host | OMS machinery, CS/DCLF producers |
| New ORG work | none (optionally multi-epoch attach) | large: variables, descriptors, configuration, adopt | R2 (`FindTexture` in OMS `NamedVersions`), R3 (multi-epoch staged attach) |
| Atomic across CS stages | emulated in CS | yes | yes |
| Atomic with DCLF epochs | no | no (unless DCLF moves too) | yes |
| Mechanisms left | 2 (this + DCLF) | 2 (unless DCLF moves) | 1 |
| Deletes `RequestSize`, `AtNextBegin` users, prepass checks, `Service` poll | yes | yes | yes |
| Deletes DCLF's frame-start wait | no | no | yes |
| Fits "graph primitives over bookkeeping" | weakly | strongly | strongly (OMS is the graph-side library) |
| Risk | low | medium-high | high in the DCLF steps |

---

## 5. Recommendation

**Target: option (c), built from small graph-side primitives (the useful parts of (b)), with CS declarations shaped like (a).**
In short: the mechanism is ORG/OMS; the declarations and event sources are CS; DCLF joins last.

Reasons:
- The hard parts (record against pending versions, atomic multi-epoch adoption, closure by declared slots, alias placement) need host
  knowledge. They belong in ORG/OMS as primitives, not in a CS list.
- OMS already has the assembler, the artifact graph, `NamedVersions` and growth producers. Extending them to textures is smaller than a
  new configuration system in ORG.
- One mechanism is the stated goal. DCLF already reads CS-published textures, so atomicity has to include DCLF eventually.
- Doing DCLF last keeps its proven path untouched until the CS side is solid.

### 5.1 Migration order

Each step leaves the tree working and testable with the existing cs-test configs (`resize`, `menu`).

1. **Write the rule and arm the check.** Document R1 in `org-native-frame-graph.md`. Add a development-build fatal check: any version
   publish, `Register`, `Refresh` or build attach on the render thread outside the Begin adoption point is a tap error with the caller
   named. Start it as a counted warning so the census lists every remaining site. No behaviour change.
2. **ORG/OMS: name texture versions.** Implement `FindTexture` in `OMS/GrowableBufferSet.h:NamedVersions` (or a sibling set for
   textures). Let `FrameAnchors.cpp:SegmentHostData` carry an `IResourceVersions` (null today = current, so no behaviour change).
3. **ORG: atomic multi-epoch staged attach.** `GraphHost::AttachStagedEpochGraphs(span<epoch>)`: checks all, swaps all, or refuses all.
   Plus `EpochsDeclaring(slot)` for the closure. Unit tests in ORG.
4. **CS: the prepared set, behind today's API.** Re-implement `RequestSize` on top: `VersionedTexture::Make` off the render thread, birth
   fill for `History`, member stages recorded against the pending versions, the whole set adopted at Begin through step 3. Delete
   `ApplySizeRequests`'s on-thread allocation and the per-stage version lag in `Schedule`. Features still call `RequestSize` each frame,
   but the cross-stage mixing (2.4) is gone. Test with `resize`.
5. **CS: variables and event sources.** A small fixed registry: render size and dynamic ratios (written where `ConfigureUpscaling`
   computes them, as an event on change), display size (`EngineTargets::EndSet`, `NativeDevice::Resize`), upscaler method, HDR on/off and
   output target, engine shadow map size/format. For settings: one central change point covering every writer (section 3.4 item 7). The
   menu's `FeatureListRenderer` edit detection, preset/undo/`LoadSettings` paths and the scene settings manager each raise "feature X
   settings changed"; the registry then projects the size-relevant fields. Features read the adopted values, not live settings, for those
   fields.
6. **Migrate features one at a time**, each deleting its in-frame checks: EHF (extent and shape), SSGI, SSS shadows, Terrain Blending,
   ImageSpace `Frame::Resources` (and `CheckChainSteps`/`CheckImports`), LLF clusters, post effects key, Sun Rays, Volumetric Lighting,
   LLF local shadows, Upscaling method (delete `previousUpscaleMode` and the mid-frame `DrainCommandRing`), HDR composite inputs. Fix the
   setup-only features (SSS working textures, Dynamic Cubemaps, Volumetric Shadows copies, LLF buffers) to declare their dependency on the
   engine target or shadow map variables, so they become correct rather than lucky.
7. **Delete the old paths.** `RequestSize`, `RequestExtent`, `ResizeGeneration`, `BumpResizeGeneration`, `FrameAnchors::Service`'s
   resize poll, `PublishedResources::Refresh`'s resize role, `Rearm`-for-reset, `Frame::Resources`' `fresh`, Terrain Shadows' `AtNextBegin`
   switch, ImGui's `PublishPages` (both become set members). Keep `AtNextBegin` only if something legitimate is left; otherwise delete it.
   Turn step 1's check fatal.
8. **Dynamic resolution as viewports (optional, recommended).** Size frame transients and history textures for the display size; a
   render-size change becomes a constant/viewport change in the adopted set, not a resize. This removes the most frequent resize, the
   alias-arena growth risk (R9), and most history resets.
9. **Engine imports.** Make announced engine target changes events (they already are, step 5). Then remove or restrict `JoinSlot`/
   `EnsureViews` mid-frame publishing per the answer to question 3.
10. **DCLF joins.** Stage DCLF's epochs (`BuildManager::Desc::staged` instead of `external`) so they switch with their recordings; delete the
    `kStaleBuild` wait. Then make DCLF's snapshot a fragment of the frame revision and its toggles and graph conditions variables, so one
    adoption at Begin installs both.

### 5.2 Risks

- **Recording count.** CS records per description and per frame slot (four slots). A set with N member stages needs 4N recordings before
  it can be adopted. Frequent events (a slider drag) supersede often. Latest-wins plus coalescing (prepare at most one set at a time,
  re-prepare from the newest values when it finishes) keeps it bounded. The starvation rule must not fire on a long drag **(inferred)**.
- **Memory peak.** During preparation both sets' textures live: old ones bound by frames in flight, new ones pending. A display-size
  change doubles the engine-size textures briefly. Fine on 8 GB, worth measuring on the RX 6600.
- **Frame arena placement** (R9). Re-made frame transients may not fit next to the live ones. Step 8 avoids most cases.
- **Settings writers.** Missing one writer means a setting that does nothing until the next unrelated event. A development check could
  compare the projected fields once per frame and report a change that raised no event (a test-only poll, not part of the frame).
- **Scene-layer settings (inferred).** If the scene settings manager interpolates a size-relevant field across a transition, each step is
  a resize. Size-relevant fields should be excluded from interpolation or quantised.
- **Guest own-epoch anchors** (HDROutput, FrameGeneration) take builds mid-frame today. They must become staged members, or the atomic set
  cannot include them.
- **DCLF regression risk** in step 10. Keep it last and separate.
- **Shader reloads** are `RequestAllSynchronous` today (host thread held for the pipeline swap). They fit the set as "every epoch is a
  member", but the synchronous swap needs care **(not investigated in depth)**.

### 5.3 What I am unsure of

- Whether DCLF's epochs actually bind CS-published texture versions in their recordings (so they must be members of a resize set), or
  read them late-bound. The "declared view mismatch" log in `parity/status.md` suggests they bind them. Not verified in code.
- Whether `AttachStagedEpochGraph` refusals are frequent enough in practice to matter for an all-or-nothing attach.
- How the frame alias arena behaves for a larger re-made frame transient while the old one is live. The header says placements never
  move and spans only grow; I did not read `FrameAliasArena`'s placement code.
- Whether native mode creates engine targets as ORG textures it can `Make` ahead (inferred from `EngineTargets.h`), so a display-size
  change can be fully prepared before the engine sees it.
- Whether `ApplySizeRequests` allocations cost a visible hitch today (not measured).
- The exact list of settings writers beyond those named in 3.4.

---

## 6. Clarifying questions

1. **Where should the cascade live?**
   (a) a CS registry on today's ORG; (b) inside ORG's `GraphHost` as a configuration primitive; (c) OMS machinery (assembler, artifacts,
   `NamedVersions` extended to textures) with small ORG additions, shared with DCLF.
   *Recommended: (c).*

2. **How far to unify with DCLF, and when?**
   (a) not at all, two mechanisms; (b) stage DCLF's epochs only (removes its frame-start wait), keep its snapshot separate; (c) full: DCLF's
   snapshot becomes a fragment of one frame revision, toggles and graph conditions become variables.
   *Recommended: (c) as the end state, done last; (b) first.*

3. **Must engine-driven texture changes go through the same system?**
   (a) yes, announced changes (window resize, `ResizeBuffers`) become events and any discovered mid-frame import is fatal until its source
   is found; (b) announced ones yes, keep `JoinSlot`/`EnsureViews` mid-frame publishing as the one documented exception; (c) leave engine
   imports as they are.
   *Recommended: (a), with (b) as an interim step.*

4. **Is late adoption acceptable for user-driven setting changes?** The set is adopted once its builds and recordings are ready, typically
   one to a few frames after the change; during a slider drag only the latest value is prepared.
   (a) yes, any number of frames late, latest wins; (b) at most N frames, then fatal; (c) no, must apply on the next frame (forces waiting).
   *Recommended: (a), with the starvation rule as the only bound.*

5. **May a size change show one frame of cleared history?** New history versions are born cleared and the first frame on them runs
   without temporal accumulation (a brief shimmer in TAA, fog, SSGI).
   (a) yes; (b) no, resample the old history into the new size on the GPU as part of the birth fill.
   *Recommended: (a).*

6. **Should dynamic resolution stop resizing textures?** Size render-size textures for the display size and change only viewports and
   constants.
   (a) yes; (b) no, keep exact-size textures and resize on every ratio change.
   *Recommended: (a). It removes the most frequent resize and most of the alias-arena risk, at some VRAM cost when rendering below display size.*

7. **How are settings changes detected?**
   (a) each writer raises an event (menu edit detection, presets, undo, `LoadSettings`, scene layers, cs-test), plus a development-only
   check that catches a missed writer; (b) wrap size-relevant fields in a `Setting<T>` type that raises the event on write; (c) one central
   per-frame diff of the projected fields outside the stage path.
   *Recommended: (a) now, (b) for new code.*

8. **Must the set be atomic across every stage, or is per-stage switching acceptable for changes local to one stage?**
   (a) always atomic over the derived closure (a local change has a closure of one stage anyway); (b) per stage, as today.
   *Recommended: (a).*
