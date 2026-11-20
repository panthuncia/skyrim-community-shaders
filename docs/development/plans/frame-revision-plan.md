# Frame revisions: one mechanism for resizes and graph changes

Plan, 2026-11-18. It implements option (c) of `_notes/central-resize-events.md` (the investigation) in its ten steps, with the
user's decisions below. It was written read-only against worktree `org-port-worktrees/test-runner`, CS branch `org-native/phases`
at `8dd3e865e`, ORG `6028568`, OMS `0185120`. Nothing was built or run.

Updated 2026-11-19, read-only, against `integ/hard-ports` at `5668ec493`: upstream sync batch 16, the parity round on
`org_integration`, and the hard ports (Physical Sky, ViewAtmosphere, Advanced Skin in DCLF, Physical Lens, Tree Wind for object
LOD). Their sites are in sections 3.2, 5, 6.4, 7.12, 8 and 9. Each port's design note is in `docs/development/plans/`.

Paths are relative to the worktree. `DCLF/` is `src/Features/DrawcallLimitFix/`. `ORG/` is `extern/OpenRenderGraph/`. `OMS/` is
`extern/ORGModuleServices/include/ORGModuleServices/`. Citations are `file:function`. **(inferred)** marks what was concluded, not read.

Read before starting: this file's sections 1, 2, 4 (your step) and 10. Read the investigation's sections 1 and 2 once for background.

---

## 1. Goal and end state

**The model.** Everything a recording binds changes in one place: the frame's Begin adopts the newest fully prepared *frame
revision*. Nothing inside a frame asks "did a size change". Nothing inside a frame queues work for the next one.

- Inputs are **variables**: display size, render size and dynamic ratios, upscale method, HDR and its output target, engine shadow map
  size and format, frame-wide toggles, DCLF's toggles and graph conditions, and the size- or shape-relevant feature settings.
- A changed write to a variable is an **event**. Writing the same value is not an event.
- **Dependents** declare which variables they read: a texture's description, a client's pass set (its stage's graph), a program.
- An event wakes the **revision lane**, off the render thread. It recomputes the dependents of the changed variables, makes new
  texture versions (born cleared), asks for staged builds, and records every member stage against the pending versions.
- When every fragment is ready, the lane seals a **prepared revision**. Latest wins. A newer event supersedes a revision still in
  preparation.
- At **Begin**, the frame adopts the latest sealed revision whole: its versions become current, its staged graphs attach, every member
  stage's schedule switches, its variable values become the frame's. Or nothing changes and the frame runs the last revision.
- Late-bound data (DCLF frame values, light contexts, portal programs, latch regions) is outside this. It may change any time, as today.
  The ports added more of it: frame-value pools (Advanced Skin's material table at t38), per-draw latch handles (the sky and water
  families' `PhysSkyOrgHandles`), run gates for on-change passes (Physical Sky's disk phase LUT, Physical Lens's caches), GPU
  argument seeds in a latch block, and readback results (the lens focus, the adaptation). None of them is a revision input.
- What replays: a CS stage is one revision-driven epoch. Its recordings are made once per description and frame slot and submitted
  again every frame. So everything a CS stage records must be replayable (rule R9, section 2.2).

**Vocabulary.**

| Term | Meaning |
|---|---|
| Variable | A typed, named input with a value per revision. `org::services::Variable<T>` (handle), held in a `VariableSet`. |
| Event | A write that changed a variable's value. It bumps the variable's serial and wakes the revision lane. |
| Setting | `CS::Setting<T>`: a feature settings field that is a variable. Assigning it raises the event. |
| Derived variable | A variable computed from others on the lane (CODBloom's used mip count from its blend factors). It raises an event only when its own value changes. |
| Dependent | A texture description, client shape or program declared as a function of variables. |
| Frame revision | One `org::async::AssembledRevision` with these fragments: inputs (variable values), texture versions, buffer versions (DCLF), builds (staged generation per epoch), recordings (per member epoch and frame slot), scene (DCLF's snapshot). |
| Closure | The member epochs of a change: those rebuilt, plus those whose graphs declare a slot that gets a new version (`GraphHost::EpochsDeclaring`). Derived, never listed by hand. |
| Prepared | Every fragment of the revision is ready: versions made and filled, builds staged, recordings done. |
| Adoption | `Frame::Revision::AdoptLatest()` at Begin. All of it or none of it. |
| Born cleared | A new version of a `History` texture is zero-filled before it is ready, and the revision marks it first-frame. |

**Diagram.**

```
 writers (menu, presets, undo, LoadSettings, scene layers, cs-test, engine hooks, Upscaling, HDR)
        |  Setting<T>::operator= / Variable::Set   (any thread, lock-free, equality-checked)
        v
 VariableSet --wake--> revision lane (OMS SerializedTaskPump, off the render thread)
                         | 1 evaluate dependents of the changed variables
                         | 2 make texture versions (VersionedTexture::Make) + birth fill on the uploader
                         | 3 request staged builds for changed client shapes
                         | 4 closure = rebuilt epochs + GraphHost::EpochsDeclaring(changed slots)
                         | 5 RequestEpochRecording per member x frame slot, host data names the pending versions
                         v
                 RevisionAssembler (draft -> seal) --latest wins--> sealed revision
                                                                         |
 render thread, Frame::Driver::OnBegin:  Frame::Revision::AdoptLatest()  v
     versions AdoptAll -> GraphHost::AttachStagedEpochGraphs(members) -> member schedules switch -> inputs install
     (or nothing: the frame runs the previous revision)
```

**What a feature author writes at the end.** A fog volume whose size depends on the display size, a grid setting and a depth setting.
There is no per-frame size check anywhere in the feature.

```cpp
// ExponentialHeightFog.h: the settings that set sizes are Setting<T>. Assigning one raises its event.
struct Settings {
    CS::Setting<std::uint32_t> volumetricGridPixelSize{ 8 };
    CS::Setting<std::uint32_t> volumetricGridSizeZ{ 128 };
    float fogDensity = 0.1f;  // not size-relevant: a plain field
};

// ORGExponentialHeightFog.cpp, at Setup (render thread, once).
vbufferA = Frame::DeclareTexture3D("ExponentialHeightFog::VBufferA",
    Frame::DependsOn(Frame::Vars::DisplaySize(), settings.volumetricGridPixelSize, settings.volumetricGridSizeZ),
    [](const Frame::Inputs& in) {
        const auto display = in.Get(Frame::Vars::DisplaySize());
        return Frame::Texture3DDesc{ DivideUp(display.width, in.Get(gridPixel)), DivideUp(display.height, in.Get(gridPixel)),
            in.Get(gridZ), DXGI_FORMAT_R16G16B16A16_FLOAT };
    },
    { .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV });

// In the frame (commit): read only the adopted revision.
const auto& frame = Frame::Revision::Adopted();
constants.gridSize = frame.Extent(vbufferA);                         // the size the bound version has
constants.renderScale = frame.Get(Frame::Vars::DynamicRatios());     // dynamic resolution is a constant (decision 6a)
constants.historyValid = !frame.FirstFrame(vbufferA);                // born cleared on a new version (decision 5a)
```

---

## 2. Decisions and rules

### 2.1 Decisions (final)

| # | Question (investigation section 6) | Decision | Consequence for this plan |
|---|---|---|---|
| 1 | Where the cascade lives | **c**: OMS machinery (assembler, artifacts, `NamedVersions` extended to textures) with small ORG additions, shared with DCLF | Generic code goes into OMS (`org::services`, `org::async`) and ORG. CS holds declarations, event sources, the stage producer and the adoption call. No CS-side registry of dependents survives. |
| 2 | How far to unify with DCLF | **c**: full. DCLF's snapshot becomes a fragment of one frame revision. Toggles and graph conditions become variables. Staging DCLF's epochs first is an acceptable intermediate. | Step 10 in three sub-steps: stage, then fragment, then variables. |
| 3 | Engine-driven texture changes | **a**: announced changes become events. Any discovered mid-frame import is fatal until its source is found. `JoinSlot`/`EnsureViews` mid-frame publishing only as an interim. | Step 9 turns `FrameAnchors.cpp:JoinSlot/EnsureViews` mid-frame publishing into a tap error and removes each source. |
| 4 | Late adoption for settings | **a**: any number of frames late, latest wins. The starvation rule is the only bound. | No frame-count limit. Section 7.8 states the rule. |
| 5 | One frame of cleared history | **a**: allowed. | Birth fill (zero) plus `FirstFrame`. No resampling. |
| 6 | Dynamic resolution | **a**: render-size textures are sized for the display size. Only viewports and constants change. | `RenderSize`/`DynamicRatios` are constants-only variables. Declaring a texture on them is a registration error (step 8). |
| 7 | Settings detection | **a**: each writer raises an event, plus a development-only missed-writer check. End state for size/shape fields: `Setting<T>`, which raises on write. | Step 5 adds the writer shims and the check. Step 6 ports fields to `Setting<T>` per feature. Step 7 deletes the shims. Port list in section 6. |
| 8 | Atomicity | **a**: always atomic over the derived closure. | Step 4 replaces per-stage switching for versions and builds. |

**Final form: no scaffolding.** Every intermediate this plan introduces is named where it appears, with the step that deletes it.
Section 8 is the checklist.

### 2.2 Rules the work must keep

- **Errors are fatal.** No retries, fallbacks, timers or holding mechanisms. A preparation that fails calls `stl::report_and_fail`
  naming the variable, the resource and the epoch.
- **Versions publish only at frame boundaries.** After step 7, any adoption of a bound version outside `AdoptLatest` is fatal.
- **The render thread never waits.** No step may add a wait. Step 10a removes the one that exists.
- **Graph primitives over app bookkeeping.** Closure comes from ORG (`EpochsDeclaring`). Waits are artifact requirements. No pending
  lists in CS.
- **Remove the trigger.** Prefer not producing a change (display-size textures, 1-texel stand-in versions for disabled features) over
  handling it.
- **Async builds switch in at the next snapshot.** Builds are staged and adopted with their revision.
- **One code path for native and guest** wherever possible. Differences are listed in section 7.
- **Plain D3D11 mode (`CS_ORG=0`) keeps working untouched.** `Setting<T>` must work with no graph: writes store the value and raise
  nothing. Every new CS file keeps the `#if`/stub pattern the render-graph files use (for example `GraphResources.cpp`'s stubs).
- **Never touch `extern/dxvk`.**
- **Bound vs late-bound (R1).** Bound: anything a recording binds (declared versioned textures and buffers, graph structure, programs,
  prepared constants). It changes only by adoption. Late-bound (bindless index or latch, stand-in declaration): may change any time.
- **Replayable stages (R9).** A CS stage's recording is replayed (`GraphHost.cpp`, "A revision-driven epoch's recording is not
  replayable (a pass with lifecycle effects, or a late-bound slot)", fatal). A recording is kept only when
  (`RenderGraphPersistentExecute.cpp`, `keepable`): every packet is replayable (no submission, completion or abandonment callback,
  no external signal, no dependency lifecycle effect: `PreparedPass::IsReplayable`), every pass recorded at execution is
  replayable too (the upscaler evaluate is one), the segment has no per-frame resource (a swap chain image) and its admission has
  no incoming effects. A readback capture (`PreparedPass::WithTrailer`, the `IReadbackService` plans) is never replayable.
  Per-run work in a replayed recording reads its counts from a latch or a GPU buffer seeded from one, as Physical Lens's indirect
  work does. Readbacks and versioned uploads stay out of CS stage recordings, or ORG gives them a replayable form (Q11).

---

## 3. Where things are today

A compact map. Mechanisms are described in the investigation; this lists where they live and what was corrected.

### 3.1 ORG and OMS

| Mechanism | Where |
|---|---|
| OMS location | **Separate submodule** `extern/ORGModuleServices` (remote `panthuncia/ORGModuleServices`, branch `org-rebuild`). Its HEAD `0185120` is detached, two commits ahead of local `org-rebuild` (`e56b7f6`). Not inside ORG. |
| Versioned texture | `ORG/include/Resources/VersionedTexture.h`: `Make` (any thread), `Publish`/`Adopt` (owner thread), `MakeImage`. `ORG/src/Resources/VersionedResource.cpp:SelectVersion` picks the version named by host data's `IResourceVersions`, else current. `MakeVersion` numbers versions at make time and enforces one shape class. |
| `IResourceVersions` | `ORG/include/Resources/VersionedResource.h`: `Find` (buffers), `FindTexture` (default returns null). |
| `NamedVersions` | `OMS/GrowableBufferSet.h:NamedVersions`: buffers and latch blocks only, `Serial`, `AdoptAll`, `Bind`. Built only by `GrowableBufferSet` (friend). No `FindTexture`. |
| Revision assembly | `OMS/Async/RevisionAssembly.h`: `RevisionFragment` (`Resolve`/`Fail`, requirements = closure), `RevisionAssembler` (`Begin`/`Set`/`Seal`/`Collect`/`TrySelect`/`AcquireActive`). Unchanged slots inherit, newest complete wins, a newer one never cancels an older incomplete one. |
| Growth producers | `OMS/Async/VersionGrowthArtifact.h:PostVersionGrowth`, `OMS/VersionedBufferGrowth.h:GrowVersions`, `OMS/VersionFill.h:ZeroFill`, `OMS/BackingPool.h`. Buffers only. |
| Staged graphs | `ORG/include/OpenRenderGraph/GraphHost.h:StageEpochGraph`, `AttachStagedEpochGraph(epoch)` (one epoch), `StagedGeneration`, `DropStagedEpochGraph`. Refused only when the host configuration changed since the build started, or a newer build runs. While a graph is staged the attached graph takes **no new recording** (`ORG/docs/persistent-epochs.md`, "Staged graphs"). |
| Recordings | `GraphHost.h:RequestEpochRecording(epoch, hostData, done, copy, generation)`, `EpochRecordingCurrent` (build), `EpochRecordingVersionsCurrent`/`EpochRecordingStaleVersion` (`ORG/src/Render/RenderGraph/RenderGraphPersistentTickets.cpp:PersistentRecordingVersionsCurrent` compares each recorded slot's version number with the current one). A recording made against a pending version reads "not current" until that version is adopted, then "current". No ORG change is needed for that. |
| Frame alias arena | `ORG/include/Render/RenderGraph/FrameAliasArena.h`, `GraphHost.h:SetFramePositions/AddFrameTransients`. See correction 3. |

### 3.2 CS frame side

| Mechanism | Where |
|---|---|
| Twins | `src/RenderGraph/GraphResources.h` (header rules), `GraphResources.cpp:Resource::AdoptSlot/JoinSlot`, `Texture2D::Resize`/`Texture3D::Resize` (bump `BumpResizeGeneration` when not same-class versioned), `Resource::RequestExtent` (queues `AtNextBegin`), `Texture2D::RequestSize`/`Texture3D::RequestSize`. "Everything here runs on the render thread." |
| Boundary hook | `src/RenderGraph/Frame/FrameDriver.cpp:AtNextBegin` (pushes a closure), `OnBegin` (runs closures first, then `DecideKind`, native `DCLF::BuildPointAtFrameStart`, `PostChain::OnBegin`, `Recipes::Check`, `FrameAnchors::BeginFrame`). `OnBegin` is called from `Engine/LoadingScreen/LoadingScreen.cpp` (the `Renderer::Begin` hook) in both graph modes. |
| Census and tap errors | `FrameDriver.cpp:NoteTapError` (fatal unless `CS_ORG_MISMATCH_FATAL=0`; off in `CS_RELEASE_PROFILE`), `NoteLateRequest`, `LogStats` (the per-segment census line). |
| Kept textures | `src/RenderGraph/Frame/FrameResources.cpp:Texture2D` (`RequestSize` and `fresh`). Users: ImageSpace chain (TAA, bloom, adapt) and SAO history. |
| Stage switching | `src/RenderGraph/FrameAnchors.cpp:Schedule` (per declared segment: version-lag path, staged build path with `kAskedAgainLimit`, `Builds::SwitchStaged`), `RequestDeclared` (builds `SegmentHostData`, calls `RequestEpochRecording`), `SegmentHostData::TryGet` (answers only its own type), the commit check (`TapError::StaleRecording` when `frameVersionsCurrent` and the recording's versions are not current). |
| Resize poll | `FrameAnchors.cpp:Service` (reads `GraphResources::Resource::ResizeGeneration`, calls `PublishedResources::Refresh`, `StructuralRebuild(~0)`). |
| Engine imports | `FrameAnchors.cpp:JoinSlot`, `EnsureViews`, `Import`. |
| Builds | `src/RenderGraph/BuildManager.h`: `Desc::staged`/`external`/`ownerBuilt`, `SwitchStaged`, `Poll(point, Attach)`, `Builds::Request/RequestAll/RequestFor/RequestAllSynchronous/Change/Defer/DeferStaged`. |
| Published ids | `src/RenderGraph/PublishedResources.h` (rules), `PublishedResources.cpp:PublishEntry/Store/Withdraw`: a new resource under a known id is `RequestStructuralRebuild` of every graph. |
| Engine targets | `src/RenderGraph/Engine/Targets/EngineTargets.cpp:BeginSet/EndSet/Settle/Move`, called around the engine's `BSShaderRenderTargets::Create` in `src/Hooks.cpp` (the thunk with `Targets::BeginSet`). `Enabled()` = `Native::GraphMode()`. |
| Frame transients | `src/RenderGraph/Frame/Transients.cpp:Place` (callers: `Engine/Native/NativeDevice.cpp` engine target groups, `Engine/Scaleform/Passes.cpp` scratch). |
| Previous-frame reads | `src/RenderGraph/Frame/RecipeTable.h:kPreviousFrame` (15 ids; checked by `Recipe.cpp`): a stage reads a resource before the stage that writes it, so it gets last frame's contents. New: Physical Sky's `TransmittanceLut`, `SkyViewLut`, `ApLut`, `DiskPhaseLut`, read by the sky's reflection faces in the start stage before the pre-depth stage writes them (`4e219be6c`). |
| Schedule gate | `FrameAnchors.h:Scheduled(id, anchor)`. A client registered inside the frame runs nothing until a build that holds it switches in. Users: the screenshot's clean scene (`ORGScreenshot.cpp`). ViewAtmosphere's post-water composite used it until 6.21. |
| Staged buffers | `ORGPhysicalLens.cpp` `StagedBuffer::Poll`: a new ghost paths version uploads on the uploader's worker path (`WorkerOwnedDestination::PendingVersion`), the render thread polls the ticket each frame, then `AtNextBegin` adopts it and calls `NoteNewVersions`. |
| Readbacks | `org::runtime::IReadbackService` through readback passes: `ORGPhysicalLens.cpp:FocusReadbackPass` (new), `ORGHistogramAutoExposure.cpp:ReadbackPass` (now feeds the pre-exposure metering, `eb71d2e7f`), the screenshot's readback buffers (Capture anchor), and Physical Sky's lightning ring (planned, `physical-sky-port.md` section 6). |

### 3.3 DCLF

| Mechanism | Where |
|---|---|
| Revision make | `DCLF/Core/Render/Revision/Producer.cpp:IndirectDraws::Impl::MakeRevision`, `Requirements.cpp`, `Assemble.cpp:AssembleRevision/RecordingCompleted`. |
| Snapshot and adoption | `DCLF/Core/Render/Snapshot/Snapshot.cpp:IndirectDraws::AdoptSnapshot` (takes `snapshotSlot`, `StaleKindOf`, `NamedVersions::AdoptAll`, `host->NoteNewVersions`, payload install), `Impl::StaleKindOf` (build generation, build serial, targets generation, shadow format, main generation), `IndirectDraws::AdoptCurrentSnapshot(bool wait)` (the frame-start wait). |
| Build point | `DCLF/Core/Render/Revision/Coverage.cpp:IndirectDraws::BuildPoint` (`RegisterGraphBuilds`, `RefreshGraphConditions`, `BuildIfAsked`); `RenderGraphRuntime.cpp:AttachDclfBuilds`. |
| Toggles | `DCLF/Core/Settings/Toggles.h/.cpp` (`ToggleSet`, `BeginFrame`, `Adopt`, `Generation`). Conditions: `DCLF/Core/Render/Epochs/GraphConditions.cpp:RefreshGraphConditions`. |
| Published reads | `DCLF/Core/Extensions/FrameBindings.cpp:DeclareGraphTextures/DeclarePublishedRead/CheckDeclaredReads/GraphTextureIndex`. |
| Frame textures from ports | `Features/Manifest.cpp:kFrameTextureRegisters`: Physical Sky (t61-t63, t106 LUTs by published id; t64, t66, t67, t69, t105, t109-t115 from the captured D3D11 view in guest, a zero view in native), ViewAtmosphere's medium (t104, t107, t108). Reflection faces will read the reflection medium through per-epoch views (`view-atmosphere-port.md` open question 2). |
| Frame-value pools from ports | Advanced Skin's `skin-materials` table at t38 (`dclf-skin-per-geometry.md`): rows captured at the frame start in `CaptureShadingFields`, sent in `FrameValues::Kick`, `GpuTextures` replies drained at capture, every member re-put on a `GpuTextures` generation change. `kFrameBindingSlots` 5 to 6. |
| Feature buffers (planned) | Tree Wind for object LOD (`dclf-tree-wind-object-lod.md`, deferred and flagged for redesign): `Draws::Versioned` `cs.dclf.tree-lod-bend`, grown with the scene buffers and published with a sealed set; regions uploaded by a commit hook against the adopted version's tags. |

### 3.4 Corrections to the investigation

1. **OMS is its own submodule** (`extern/ORGModuleServices`, branch `org-rebuild`), not part of ORG. OMS changes are a separate commit and
   a separate submodule bump. Its checked-out commit is ahead of the local branch: check the remote before committing (section 10).
2. **DCLF's colour and faces passes do declare CS-published textures** (`FrameBindings.cpp:DeclareGraphTextures`, from
   `ColourGraphTextureIds`). So DCLF epochs are members of a closure when such a texture gets a new version. DCLF never checks texture
   version currency: no DCLF code calls `EpochRecordingVersionsCurrent`, and its `NamedVersions` answers buffers only, so its recordings
   bind whatever texture version is current when the host thread records them **(inferred from `SelectVersion` and the null
   `FindTexture`)**. That is the "declared view mismatch" line in `docs/development/parity/status.md`. Step 10b closes it.
3. **Frame alias arena and re-made textures.** A re-made frame transient gets a **new** reservation (`FrameAliasArena::ReserveTextures`)
   that starts with the span the old one reached. Ranges whose spans overlap get disjoint bytes, in a new block if needed. So the live
   old textures and the new ones never share bytes. The cost is a memory peak, not a failed plan. A plan fails only when a later build
   must widen the span into bytes another range holds (`Widen`) **(placement code itself not read)**. Only native engine targets and
   Scaleform scratch are frame transients (`Transients::Place` callers). Feature twins resized through `RequestSize` are History,
   Persistent or exported, never frame transients.
4. **Engine targets are ORG images in both graph modes**, not native only (`EngineTargets.cpp:Enabled` = `GraphMode()`; the header's
   `CS_ORG_ENGINE_TARGETS` text is stale). They are made **synchronously inside the engine's own `Create` call**, so CS cannot make
   them ahead of the engine. A second `Create` logs "CS's resources are not made for that" and runs `State::Setup` again
   (`Hooks.cpp`), so a mid-session window resize is effectively unsupported today.
5. **DCLF's frame-start wait covers more than a new build.** `AdoptCurrentSnapshot` waits for `kStaleBuild`, and also `kStaleTargets`
   (pipelines for new target formats, up to 120 s), `kStaleShadowFormat` and `kStaleMain`. Step 10 must make all four impossible, not
   just the build case. That needs step 9 first (target changes as events).
6. **Staged attach refusals** happen only on host configuration changes or a newer build (`GraphHost.h:AttachEpochGraph` comment).
   `Schedule` simply tries again next frame **(frequency not measured)**. Step 3 adds a counter.
7. **The writer/reader mixing gap is real** (`FrameAnchors.cpp:Schedule`): each stage keeps its old recording until its own new one is
   ready, independently of other stages.
8. **Feature sites** (verified per site; full table in section 5): EHF has 8 Texture3D + 4 Texture2D twins (near and far volumes).
   SSGI and Terrain Blending size from the kMAIN texture, not from dynamic resolution. ImageSpace's `BeginFrame` also runs
   `CheckWorldSteps`, `CheckStartSteps`, `CheckMenuSteps`, `CheckFlowMap`. At least five paths rebuild every graph (EHF re-enable, LLF
   local shadows, SSGI outputs, ImageSpace adapt pair, ImageSpace flow map). Upscaling's hudless texture is polled every frame with a
   `DrainCommandRing` on change (`Upscaling.cpp:CaptureHudlessColor/DestroyHudlessTexture`). Effects11's kept textures follow the display
   size. Terrain Shadows calls `Resize` directly at Begin, not `RequestSize`. IBL has no `useEnvironment` setting (it is a parameter).
   Side note, unrelated: `FrameGenController.cpp:Reconcile` has `if (faultRecoveryRequested) { if (!faultRecoveryRequested)`, so its
   inner teardown never runs.
9. **Settings writers** are more than listed: preset baselines (`State::ReapplySettingsLayers`) write without any notification;
   `SceneSettingsManager::SceneLayerGuard` writes scene-controlled features twice around every base read (save, undo capture, dirty
   check), so an event must compare values; cs-test's `setting` step (`Testing/Commands.cpp:SetSetting`) patches top-level keys only;
   the Upscaling test requests assign fields on the render thread. Section 6 has the full list.
10. **The hard ports added sites of every kind this plan handles** (2026-11-19): new `RequestSize` users (ViewAtmosphere's atlas,
    Physical Sky's AP shadow, both planned), clients registered inside the frame with a schedule gate (ViewAtmosphere's composite,
    the screenshot's clean scene), a staged buffer adopted through `AtNextBegin` (Physical Lens's ghost paths), readback passes
    inside the post stage (the lens focus, the adaptation), and previous-frame reads of History textures (Physical Sky's LUTs).
    They are rows in section 5 (6.9, 6.20 to 6.23) and items in 7.12. The post stage now fails with the R9 error when the Post
    Processing chain runs on the graph (it held for Physical Lens until `aa221de9b`). The readback passes are the obvious
    candidates **(inferred; which pass trips it is being found separately)**. Either way it is the conflict this plan must settle
    before step 4 records stages against pending versions (Q11).

---

## 4. The ten steps

**Execution order:** 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, as numbered, with one rule from step 8 pulled forward: from step 6 on, every
migrated feature sizes render-size textures for the display size (decision 6a). Reason: otherwise each render-size feature would be
migrated twice. Step 8 keeps the frame-wide sites, the enforcement and the measurement.

Steps 2 and 3 touch disjoint submodules and can run in parallel. Inside step 6, features with disjoint files can run in parallel,
capped by game launches (section 10). Everything else is serial.

Every step ends with: the tree builds (Perf-Fast, plus `-DSC_UNITY_BUILD=OFF` for changed files before a push), and its listed runs
pass. The standard set is `guest-quick`, `native-quick`, `d3d11+quick`, `native-d3d12-quick`; "standard set" below means these four.

---

### Step 1. The rule and a counted check

**Purpose.** Write rule R1 down and count every place that breaks it, so later steps have a list and a number that must reach zero.

**Scope.**
- Docs: a dated section in `docs/development/org-native-frame-graph.md` ("Frame revisions (date)"): R1, the adoption point, the
  vocabulary of section 1, and a link to this plan.
- ORG (small): a development hook on version adoption. `ORG/include/Resources/VersionedResource.h`:
  `using AdoptionObserver = void (*)(const VersionedResource&, const ResourceVersion&) noexcept;` and
  `static void SetAdoptionObserver(AdoptionObserver)`, called from `VersionedResource::AdoptVersion` (not from `PublishLate`, which is
  late-bound by design). One relaxed atomic load when unset.
- CS: `src/RenderGraph/Frame/BoundaryCheck.{h,cpp}` (new; keep it out of `FrameAnchors.h`):
  - `Frame::Boundary::Scope` (RAII) marks "inside the adoption point". `FrameDriver.cpp:OnBegin` opens it around its whole body for now.
  - The observer counts adoptions on the render thread outside a scope, by resource name and by caller tag.
  - `Frame::Boundary::NoteLateChange(std::string_view what, std::string_view caller)` counts build requests and registrations made
    inside the frame: call it from `FrameAnchors.cpp:Register/Refresh/Unregister/Rearm` and `GraphResources::RequestStructuralRebuild`
    when on the render thread between Begin and the frame's end.
  - Staged attaches outside `Schedule` and `Poll(..., Attach::All)` at the frame end count as late attaches.
  - One census line, `[ORG] boundary: late versions N (top: name x count ...), late changes M (top ...), late attaches K`, from
    `FrameDriver.cpp:LogStats`. Development builds only (not `CS_RELEASE_PROFILE`).
- DCLF's `AdoptSnapshot` adopts buffer versions at its own frame start. Tag it `dclf` so the census separates it. **Intermediate:**
  the `dclf` tag is an allowed caller until step 10a moves DCLF adoption into `AdoptLatest` (deleted there).

**Design choices.** Hook in ORG rather than CS wrappers: it sees every path (`AdoptSlot`, `JoinSlot`, `PublishPages`, engine targets,
DCLF), so nothing is missed. Counted, not fatal: step 7 makes it fatal.

**Test.** Standard set, `resize` composed with `native-vulkan` and `guest`, `menu`, `latch-census-short`. Record the census numbers in
the dated doc section. Expected non-zero today: engine imports (`JoinSlot`), DCLF, Register/Refresh from prepasses.

**Acceptance.** No behaviour change (shots within today's noise, section 7.10). The census line appears and names the sites. ORG unit
test: an observer sees `Publish` and `Adopt`, not `PublishLate` (`ORG/tests/PersistentGraphTests.cpp` or a new
`VersionedResourceTests.cpp`).

**Risks.** Overhead of the hook: one atomic load per adoption, negligible. Thread attribution: counts only the render thread.

**Size.** Small. One agent. Serial (first).

**As built (2026-11-19, `org-native/rev-step1`).** `src/RenderGraph/Frame/BoundaryCheck.{h,cpp}`, ORG
`VersionedResource::SetAdoptionObserver`. Where it differs from the scope above, see section 9, "Decided by the agent (step 1)".

---

### Step 2. Name texture versions (record against pending versions)

**Purpose.** R2: a CS stage can be recorded against a texture version that is not current yet.

**2a. OMS: `NamedVersions` answers textures.** `OMS/GrowableBufferSet.h`:
- `NamedVersions` gains texture entries: `std::vector<std::pair<const void*, std::shared_ptr<const TextureVersion>>> m_textures` sorted
  by `VersionedTexture::Key`, and `FindTexture` override (counts misses like `Find`).
- A builder for sets that hold textures and no growable buffers: `static std::shared_ptr<const NamedVersions> NamedVersions::Textures(
  std::span<const std::pair<std::shared_ptr<VersionedTexture>, std::shared_ptr<const TextureVersion>>>)`. New serial per call.
- `AdoptAll` adopts textures too.
- `ComposedVersions` (new, `OMS/ComposedVersions.h`): an `IResourceVersions` over up to four others, first answer wins. The frame
  revision's recordings use it to name both DCLF's buffer set and the frame's texture set without merging them, so a texture change
  never makes DCLF's buffer fragment new (and the reverse).
- Unit tests in `extern/ORGModuleServices/tests/Growth/GrowthTests.cpp` (or `tests/Buffers`): find, miss, serial, `AdoptAll` with
  textures, composition order.

**2b. CS: `SegmentHostData` carries versions.** `FrameAnchors.cpp:SegmentHostData` gains
`std::shared_ptr<const org::IResourceVersions> versions;` and `TryGet` returns it for `typeid(org::IResourceVersions)`. Null (today)
means current versions. `RequestDeclared` gets an optional versions argument; `SegmentRecording` keeps the versions' serial so a
recording for other named versions is another cache entry.

**Intermediate.** None: both pieces are permanent.

**Test.** ORG unit test (new case in `ORG/tests/PersistentGraphTests.cpp`): a revision-driven epoch records with host data naming a
`Make`d texture version; the recording binds it; `EpochRecordingVersionsCurrent` is false, then true after `Adopt`. CS: standard set
(no behaviour change).

**Acceptance.** Tests pass. Nothing calls the new argument yet.

**Size.** Small. Two agents in parallel (OMS and CS are disjoint), or one. Parallel with step 3.

**As built (2026-11-19, `org-native/rev-step2`).** OMS `rev-step2-named-versions`: `NamedVersions::Textures`, `FindTexture`,
`Names(const VersionedTexture&)`, `AdoptAll` over textures, `OMS/ComposedVersions.h`, test `ORGNamedVersionsTests`. CS:
`FrameAnchors.cpp` `RecordVersions`, `SegmentHostData::versions`, `RequestDeclared(..., a_versions)`. Where it differs from the
scope above, see section 9, "Decided by the agent (step 2)".

---

### Step 3. ORG: atomic multi-epoch staged attach, and the closure query

**Purpose.** R3 and R4: attach several staged graphs as one step, and find which epochs a changed slot touches.

**Scope (ORG only).** `ORG/include/OpenRenderGraph/GraphHost.h`, `ORG/src/Host/GraphHost.cpp`:
- `AttachResult AttachStagedEpochGraphs(std::span<const uint32_t> epochs)`. Owner thread, at a boundary. Checks every epoch first
  (staged, configuration unchanged, not being submitted, no newer build). Refuses all with the first reason, or swaps all while holding
  the host thread once. Refusal drops no staged graph (unlike the single form), so the caller can decide; CS treats a refusal as fatal
  after step 4 (rule: errors are fatal).
- `std::vector<uint32_t> EpochsDeclaring(std::span<const VersionedResource* const> slots) const`. Any thread. The epochs whose attached
  **or staged** graph declares any of the slots. Kept from each graph's build: the set of versioned resolver identities its passes
  declare (the same data `PersistentRecording::versions` is filled from) **(inferred source; read `RenderGraphPersistent.cpp` near
  "the one its host data's revision names")**.
- Stats: refusals by reason, for the census.

**Design choices.** Refuse-all instead of partial attach, because a half-adopted set breaks decision 8a. Staged graphs count in
`EpochsDeclaring` because a set may both rebuild an epoch and give a slot it declares a new version.

**Intermediate.** None.

**Test.** ORG unit tests (`ORG/tests/PersistentGraphTests.cpp` or `ContributorHostTests.cpp`): two staged epochs attach together;
one with a configuration change refuses both and keeps both staged; `EpochsDeclaring` finds a slot in an attached and in a staged
graph and not in an unrelated one. CS: bump ORG, standard set.

**Acceptance.** Tests pass. CS unchanged in behaviour.

**Size.** Medium. One agent. Parallel with step 2.

**As built (2026-11-19, `org-native/rev-step3`).** ORG `rev-step3-staged-attach`: `GraphHost::AttachStagedEpochGraphs`,
`EpochsDeclaring`, `AttachRefusal` with `AttachStatsTotal`, and `RenderGraph::PersistentDeclaredVersions`. CS counts refusals in the
`[ORG] boundary:` census. Where it differs from the scope above, see section 9, "Decided by the agent (step 3)".

---

### Step 4. The prepared set, behind today's API

**Purpose.** One adoption point for every bound change CS makes: versions and staged builds of all member stages switch in together at
Begin. Features keep calling `RequestSize` each frame for now; what changes is that the work happens off the render thread and
adoption is atomic. This removes the cross-stage mixing (correction 7) and `ApplySizeRequests`' allocation at Begin.

Split in five sub-steps, serial.

**4a. OMS: texture version production.** `OMS/VersionedTextureGrowth.h` (new), mirroring `VersionedBufferGrowth.h`:
- `struct TextureVersionRequest { std::shared_ptr<VersionedTexture> slot; TextureDescription next; TextureContents contents; };`
  (`contents`: `Zero` for History, `Undefined` otherwise).
- `TextureVersionsMade MakeTextureVersions(std::span<const TextureVersionRequest>)`: any thread; makes an unmaterialized texture like the
  slot's current one at the new extent, materializes, `VersionedTexture::Make`, queues a zero fill on the uploader (as `VersionFill.h:
  ZeroFill` does for buffers) and returns a `RevisionFragment` that resolves to a texture `NamedVersions` once the fills complete.
- `PostTextureVersions` on a `StateGraph`, like `PostVersionGrowth`.
- Unit test: make, fill, fragment resolves, shape-class change throws.

**4b. CS: twins prepare off the render thread.** `GraphResources.{h,cpp}`:
- `Resource::Prepare(const Extent& next) -> PreparedVersion` for versioned twins (same shape class): builds the ORG texture request
  only. The D3D11 export is made at adoption, on the render thread, from the adopted version **(it wraps memory already made; its
  cost is to be measured, open question Q2)**.
- `Resource::AdoptPrepared(const PreparedVersion&)`: render thread, inside the adoption scope; makes the export, updates `Desc()`.
- `RequestSize` now records the wish and wakes the revision lane. It no longer calls `AtNextBegin`. **Intermediate:** `RequestSize`
  as a per-frame call (deleted in step 7 once step 6 removed every caller).
- Same for `Frame::Resources::Texture2D`'s same-class path. Its `fresh` stays until step 6 (**intermediate**, deleted in step 7).

**4c. CS: the frame revision coordinator.** `src/RenderGraph/Frame/Revision.{h,cpp}` (new). Generic parts go to OMS later only if a
second user appears; today everything generic it needs is already OMS's (`RevisionAssembler`, `SerializedTaskPump`, `StateGraph`).
- The lane: one `org::async::SerializedTaskPump` on the executor the runtime already uses for graph builds **(open question Q3: which)**.
- Slots of its `RevisionAssembler`: `kInputs` (empty until step 5), `kTextures`, `kBuffers` (CS-owned versioned buffers: Physical
  Lens's ghost paths, 4e; DCLF's buffer set joins it in 10b), `kBuilds`, `kRecordings` (one fragment per member declared segment,
  requiring the `kTextures`, `kBuffers` and `kBuilds` fragments it was recorded against).
- On a wake: collect wishes (`RequestSize` now; variables from step 5), post texture versions (4a), compute the closure with
  `GraphHost::EpochsDeclaring` over the changed slots, map epochs to declared segments, and for each member segment and frame slot call
  `RequestDeclared` with `versions = ComposedVersions{pending textures}` and the staged generation when the segment has one.
- DCLF epochs in the closure are **not** recorded here: they are external until step 10. **Known gap (today's behaviour):** they keep
  binding the texture version current when DCLF records. Closed in step 10b.
- Coalescing: at most one revision in preparation. A wish that arrives meanwhile is kept as "newest inputs" and prepared when the
  current one seals or is superseded. Latest wins.
- Seal posts to the assembler; `TrySelect` at Begin picks the newest complete one.

**4d. CS: adoption at Begin.** `Frame::Revision::AdoptLatest()` called first in `FrameDriver.cpp:OnBegin` (before `DecideKind`):
- `TrySelect`. If nothing new, return.
- `NamedVersions::AdoptAll` for the texture fragment, then `AdoptPrepared` per twin (exports).
- `GraphHost::AttachStagedEpochGraphs(stagedMembers)`; refusal is fatal (`stl::report_and_fail` naming the epoch and reason).
- For each member segment and slot: install the revision's recording as the schedule (`FrameAnchors::InstallRevisionSchedules`,
  new, replacing the per-segment staged switch for members).
- Retire the previous revision's leases after the frame's last submission (like `NamedVersions::Bind` with
  `DCLF::SubmittedRetirementPoint()`).
- `FrameAnchors::BeginFrame` then describes the frame against what is installed.

**4e. CS: staged builds join revisions.** A staged build of a CS stage (from any `Builds::Request`) becomes a `kBuilds` fragment of the
next revision, with its recordings. Delete from `FrameAnchors.cpp:Schedule`: the staged-build branch (`SwitchStaged` call, its
per-segment `stagedAsked*` counters) and the version-lag branch for versions adopted by revisions. The starvation rule moves to the
coordinator (section 7.8).
- **Intermediate:** the version-lag branch stays for slots that publish outside revisions (engine imports via `JoinSlot`/
  `EnsureViews`, native back buffer). It counts its uses per slot in the census. Deleted in step 9.
- **Intermediate:** `Builds::SwitchStaged` stays for staged epochs that are not CS declared segments (guest own-epoch anchors
  HDROutput and FrameGeneration's hudless copy). Deleted in step 7 (sub-step 7b). The FrameGeneration ticket now also holds the
  screenshot's clean scene copy (`cs.screenshot.clean`, `027887780`), so 7b moves both clients.
- **Buffer versions outside DCLF.** Physical Lens's ghost paths (`ORGPhysicalLens.cpp:StagedBuffer`) are a CS-owned
  `org::VersionedBuffer` whose new version is made on a CPU change (a new lens model), uploaded on the uploader's worker path,
  polled each frame and adopted through `AtNextBegin`. It joins revisions here: the lens model is the event, the upload is a
  `kBuffers` fragment that resolves when its tracked upload completes (the same shape as `PostVersionGrowth` for DCLF), and the
  post stage is a closure member through `EpochsDeclaring` of the paths slot. The flare constants that must match the version
  (surfaces, path count, sensor plane, focal length, pupil, coating) become the fragment's payload, read from `Adopted()`. That
  deletes the per-frame `Poll` of the ticket and the lens's `AtNextBegin` user. A cancelled upload stays fatal.

**Design choices.**
- Recordings are requested through the existing `RequestDeclared`, not a new path, so descriptions, parity and the absent-pass key keep
  working.
- Exports at adoption, not off-thread: D3D11/DXVK interop calls stay on the render thread, as `GraphResources.h` requires today.
- History is born cleared now. Features' own resets (`ResetHistories`, `Rearm`, `fresh`, `clearPending`) stay until their migration;
  they become redundant, not wrong.

**Test.** `resize` with `native-vulkan`, `native-d3d12`, `guest` (each must pass with zero `stale recording` errors and with every
volume resize logged as one adoption). `menu`, `hdr` (native D3D12), standard set, `latch-census-short`. New cs-test config
`resize-drag`: thirty `setting` steps one frame apart on `ExponentialHeightFog.volumetricGridSizeZ` (values cycling), then a shot 120
frames later. It checks latest-wins and that no starvation error fires during a drag. Add the census line
`[ORG] frame revision: prepared N, adopted A, superseded S, members per revision (max M), recordings per revision (max R), frames from
first wish to adoption (p50/p95/max)`.

**Acceptance.**
- `resize` passes in three modes; the log shows each size change adopted once, all member stages in the same frame.
- The boundary census shows zero late versions from `RequestSize` sites.
- No allocation on the render thread for those resizes (a Tracy zone `CS.Frame.Revision.Prepare` is on the lane, not the render
  thread).
- Fog readers' "declared view mismatch" lines remain only for DCLF readers (known gap).

**Risks.**
- Recording count (section 7.2). Detect: census `recordings per revision`.
- A member stage whose recording is not replayable (R9). The coordinator records every member against pending versions through
  `RequestEpochRecording`, which throws for such a recording. The post stage hits this today whenever the Post Processing chain
  runs on the graph. Step 4 cannot start until Q11 is decided and the post stage records replayable recordings again. Detect: run
  `guest-quick` and `native-quick` with the post chain on and Physical Lens's ghosts and starburst on.
- A member segment absent in this frame kind (menu): it still needs recordings for every slot, as today's "ahead" recordings. Detect:
  a `menu` run with a resize in it (add one `setting` step to `menu` while the menu is open).
- Exports: a D3D11 reader that cached the old SRV pointer. Today's `Resize` already re-makes exports, so features already re-read them.

**Size.** Large. One agent per sub-step, serial (4a can run alongside 4b).

**As built (2026-11-19, `org-native/rev-step4`).** 4a: ORG `rev-step4` (`bfd13bb`, `IUploadService::QueueTrackedStreamingTextureZeroFill`),
OMS `rev-step4` (`d2ec1c6`, `VersionedTextureGrowth.h`: `MakeTextureVersions`, `FragmentWhenReady`, `PostTextureVersions`). 4b:
`GraphResources` `Preparable`/`PlanPrepare`/`Prepare`/`AdoptPrepared`, `SetSizeWishHook`. 4c/4d: `src/RenderGraph/Frame/Revision.{h,cpp}`,
`Frame/RevisionRecordings.h` (implemented in `FrameAnchors.cpp`), `AdoptLatest` first in `OnBegin`. 4e: staged CS stage builds join
revisions (`Builds::AttachStaged` over `AttachStagedEpochGraphs`), Physical Lens's ghost paths are a `kBuffers` fragment
(`Revision::PublishBuffer`/`AdoptedPayload`). New configs `resize-drag`, `menu-resize`. Where it differs from the scope above, see
section 9, "Decided by the agent (step 4)".

---

### Step 5. Variables and event sources

**Purpose.** Inputs become variables with events. The lane reacts to events instead of `RequestSize` wishes.

**5a. OMS: the variable set.** `OMS/Variables.h` (new, `org::services`):
- `VariableSet`: `template <class T> Variable<T> Declare(std::string name, T initial, VariableTraits)` (setup only),
  `template <class T> bool Set(Variable<T>, T)` (any thread, lock-free; equality-checked; on change bumps the variable's serial and calls
  the wake), `Snapshot Take() const` (immutable values and serials), `ChangedSince(const Snapshot&)`.
- `VariableTraits{ bool sizes = true; }`: a constants-only variable (`sizes = false`) may not be a dependency of a texture description
  (checked at `DeclareTexture`, a registration error). Used for `RenderSize`/`DynamicRatios` (decision 6a).
- Derived variables: `Derive<T>(name, deps, fn)` evaluated on the lane; its event fires only when its value changes.
- Unit tests in `extern/ORGModuleServices/tests/` (new `Variables/` folder): equality, serials, derived, concurrent writers.

**5b. CS: frame-wide variables.** `src/RenderGraph/Frame/Variables.{h,cpp}` (new; narrow header so features do not pull in recipe
headers), namespace `RenderGraph::Frame::Vars`:

| Variable | Type | Writer (event source) |
|---|---|---|
| `DisplaySize` | `Extent2D` | `EngineTargets.cpp:EndSet`; `NativeDevice.cpp` resize path; `OwnedFrame.cpp:Present` (`needsResize`) |
| `RenderSize`, `DynamicRatios` | `Extent2D`, `float2` (constants only) | `Upscaling.cpp:ConfigureUpscaling`, which writes `graphicsState` ratios each frame today: `Set` raises only on change |
| `UpscaleMethod` | enum | `Upscaling` settings (`upscaleMethod`/`upscaleMethodNoDLSS` through `GetUpscaleMethod`) |
| `HdrEnabled`, `HdrOutputTarget` | `bool`, identity | `HDRDisplay.cpp:BeginRenderFrame` (it latches `hdrEnabledForFrame` today), `HDRDisplay::PostChainTarget` |
| `PresenterFormat` | DXGI format, HDR10 | `Upscaling.cpp:BeginRenderFrame`, from `FrameGen::ActivePresenter().Surface()` (added in 6.14) |
| `EngineShadowMap` | size, format, slices | the engine shadow map's creation (find the creation point; open question Q6) |
| `ReflectionCubemap` | size | engine target creation (`EngineTargets`) |

The lane reads `RequestSize` wishes **and** variable changes. **Intermediate:** both inputs, until step 7 deletes `RequestSize`.

**5c. CS: settings events from every writer (the interim of decision 7a).** `src/Feature.h/.cpp`:
- `virtual SizeProjection Feature::ProjectSizeSettings() const` (default: empty). It returns the size- and shape-relevant fields as a
  small value with `operator==`. **Intermediate:** each feature's projection is deleted when its fields become `Setting<T>` in step 6.
- `void Feature::SettingsWritten(std::string_view writer)`: compares the projection with the last one raised; on a change sets the
  feature's settings variable (`Vars::FeatureSettings(feature)`, a serial). **Intermediate**, deleted in step 7.
- Call `SettingsWritten` from every writer class in section 6.2.
- Development-only missed-writer check (permanent, section 6.3).

**5d. CS: `CS::Setting<T>`.** `src/Utils/Setting.h` (new, tiny, no ORG includes; it is included by feature headers). Design in
section 6.1. Includes the JSON serializer, the `Util::` widget overloads, the scene catalog generator support
(`cmake/generate_scene_settings_catalog.py`) and the scene widget interceptor's raw-write notification. No field is ported in this step,
except one pilot field (`ExponentialHeightFog::Settings::volumetricGridSizeZ` stays plain until 6.1; the pilot is a test-only field in
`GraphResourcesSelfTest.cpp` or a DevBench toggle) to prove the generator and JSON paths build.

**Also in step 5:** cs-test's `setting` step accepts nested keys (`PostProcessing.CODBloom.MipBlendFactor[3]=0.2`), so post effects can
be tested in step 6. `Testing/Commands.cpp:SetSetting`.

**Test.** Standard set. `resize` in three modes (now driven by the EHF projection event, not only `RequestSize`). `upscalers` composed
with `native-vulkan` (each method change logs one `UpscaleMethod` event and one adoption). `hdr` (one `HdrEnabled` event each way).
Turn on the missed-writer check in all runs; it must stay silent.

**Acceptance.** Every frame-wide variable logs its events (one line per event, rate-limited). The missed-writer check reports nothing
in `menu`, `resize`, `hdr`, `upscalers`. Plain D3D11 (`d3d11+quick`) unaffected.

**Risks.** Spurious events from `SceneLayerGuard` round trips: the equality check absorbs them. Detect: event counts in a `menu` run
with no edits should be zero after load.

**Size.** Medium to large. 5a and 5d can run in parallel (OMS vs `src/Utils`, `cmake/`); 5b and 5c after 5a, in parallel with each
other (disjoint files except `Feature.h`, which only 5c touches).

**As built (2026-11-19, `org-native/rev-step5`).** 5a: OMS `rev-step5` (`d48c924`, `Variables.h`: `VariableSet`, `Variable<T>`,
`VariableSnapshot`, `Derive`, `RequireSizes`; tests in `tests/Variables/`). 5b: `Frame/Variables.{h,cpp}` (typed `Set...`/`Adopted...`
per variable, `RaiseFeatureSettings`, `RaiseSetting`), `Frame/VariablesLane.h`; a revision carries the snapshot it started with in
`kRevisionInputs`, and a variables-only revision is adopted in the Begin that starts it. 5c: `Feature::ProjectSizeSettings`,
`SettingsWritten`, `ApplySettings`/`ApplyDefaults`, `SettingsWriteBatch`; the check is `CS_ORG_MISSED_WRITER_CHECK` (on by default).
5d: `Utils/Setting.{h,cpp}` (`CS::Setting<T>`, `CS::SettingRegistry`), widget overloads in `Utils/UI.h`, the generator unwraps
`CS::Setting<T>`, the interceptor's `Guard::Finish` calls `RaiseAt`; pilot in `GraphResourcesSelfTest.cpp`. Nested cs-test keys in
`Testing/Commands.cpp:SetSetting`. Native frame generation step 9 (`PresentationConfig` as a variable, input producers) landed with
it. See "Decided by the agent (step 5)" in section 9.

---

### Step 6. Migrate features, one at a time

**Purpose.** Each feature declares its resources and client shapes on variables, reads the adopted revision in the frame, ports its
size/shape settings to `Setting<T>`, and deletes its in-frame checks and resets.

**Scope per feature.** Section 5 is the list and the order. Each migration is one sub-step (6.1, 6.2, ...), one commit, one agent.

**The CS declaration API** (lands with 6.1, in `src/RenderGraph/Frame/Declare.{h,cpp}`, narrow header):
- `Frame::DeclareTexture2D/3D(name, DependsOn(...), DescFn, GraphResources::Options)` returns a handle the feature keeps. The lane
  evaluates `DescFn` on a change of a dependency. Same shape class: a new version. Other shape class: a new resource, which is a
  structural change of the readers' graphs, prepared as staged builds inside the same revision.
- `Frame::DeclareClient(clientId, DependsOn(...), MakeClientFn)`: the client's registration is a function of variables. A change builds
  the stage's graph (staged) inside the revision. This replaces `FrameAnchors::Register/Refresh/Rearm` calls from prepasses.
- `Frame::DeclareProgram(name, DependsOn(...), DefinesFn)` for shader-define changes (SSGI's `recompileFlag`, SSS and Dynamic Cubemaps'
  `programsDirty`, HDR's `programDirty`).
- `Frame::Revision::Adopted()`: `Get(variable)`, `Extent(texture)`, `FirstFrame(texture)`, `Has(texture)`.
- **Existence:** a disabled feature keeps its slots with a 1-texel version of the same shape class, so readers keep declaring them and no
  graph is rebuilt (EHF, SSGI, LLF local shadows). This removes every "publish under an existing id" structural rebuild.

**Per migration, the feature:**
1. Ports its size/shape fields to `CS::Setting<T>` (section 6.4) and deletes its `ProjectSizeSettings` entries.
2. Declares textures, clients and programs with their dependencies. Render-size textures are sized for `DisplaySize` (decision 6a);
   the dynamic ratio goes to constants. Shader changes needed for that (UV scale) are part of the migration.
3. Reads sizes for constants, group counts and viewports from `Adopted()`, never from live settings or `Desc()` of a twin in flight.
4. Uses `FirstFrame` for temporal reset. Deletes `ResetHistories`, `Rearm`, `fresh` users, `clearPending`.
5. Deletes its prepass polling, its `RequestSize` calls, its `Register` re-calls and any `DrainCommandRing` for resizes.
6. Deletes its `FrameAnchors::Scheduled` gate, if it has one: the client and every frame-wide value that tells other shaders it
   runs (ViewAtmosphere's `PostWaterComposite`) switch in the same adoption, so there is no frame where one is on and the other
   is not.
7. Adds its change to a cs-test config (`resize` grows one block per feature, or a feature config) and runs it.

**Intermediate.** None new. Each migration deletes the feature's share of earlier intermediates.

**Test per migration.** The feature's config block in three modes (`native-vulkan`, `native-d3d12`, `guest`), standard set,
`latch-census-short` once per three migrations. Shots of the feature before and after a change compared against the pre-migration
build (section 7.10). Two open defects make some baselines unusable (`dclf-open-defects.md`, "Graph-mode port gaps"): guest kMAIN
goes non-finite with the fog's volumetric fog on, and native's fog volumes hold almost no fog. Until they are fixed, 6.1 and
6.21 compare against plain D3D11 shots and the fog's debug views (`CS_ORG_FOG_DEBUG`, `CS_ORG_VIEW_ATMOSPHERE_DEBUG`), not
against the pre-migration graph build.

**Acceptance per migration.** Zero `stale recording`, zero `structural rebuild` lines caused by the feature's change, the boundary
census shows none of its sites, the feature's grep in section 8 is empty.

**Risks.** A reader in another feature or in DCLF that took a size from the producer's live state. Detect: grep the producer's
accessors (`ORGExponentialHeightFog::Slices` and similar) and move them to `Adopted()`.

**Size.** Large overall; each migration small or medium. Parallel across features with disjoint files, at most three agents at once
(game launches and builds, section 10).

**As built (2026-11-19, 6.1, `org-native/rev-step6-1`).** The declaration API is `src/RenderGraph/Frame/Declare.{h,cpp}` (its
coordinator side is `Frame/DeclareLane.h`). Its shape, for 6.2 to 6.23:

```cpp
namespace RenderGraph::Frame {
// Dependencies: frame-wide variables (Vars::DisplaySize(), Vars::HdrEnabled(), ...) and bound CS::Setting fields.
Dependencies DependsOn(const D&... a_dependencies);
class Inputs {  // one revision's values
    template <class T> T Get(Vars::Var<T>) const;
    template <class T> T Get(const CS::Setting<T>&) const;  // T up to 32 bytes
};
DeclaredTexture2D DeclareTexture2D(std::string name, Dependencies, std::function<D3D11_TEXTURE2D_DESC(const Inputs&)>, const GraphResources::Options&);
DeclaredTexture3D DeclareTexture3D(std::string name, Dependencies, std::function<D3D11_TEXTURE3D_DESC(const Inputs&)>, const GraphResources::Options&);
// DeclaredTexture<Twin>: operator* / -> give the versioned GraphResources twin passes declare.
template <class Key>
DeclaredClient DeclareClient(std::string id, Dependencies,
    std::function<std::optional<Key>(const Inputs&)> key,               // nullopt: no client
    std::function<bool(const Key&, FrameAnchors::ClientDesc&)> make);  // false is fatal
// DeclaredClient::Invalidate(): the key reads state that is not a variable yet and it changed.
class AdoptedRevision {
    template <class T> T Get(Vars::Var<T>) const;
    template <class T> T Get(const CS::Setting<T>&) const;
    std::array<std::uint32_t, 3> Extent(const DeclaredTexture<Twin>&) const;  // the bound version's
    bool FirstFrame(const DeclaredTexture<Twin>&) const;                     // first frame of a new version
    bool Has(const DeclaredTexture<Twin>&) const;
};
namespace Revision { const AdoptedRevision& Adopted(); }
}
```

- Declarations are evaluated when a revision starts (`Revision.cpp:StartPreparation`), against the snapshot that revision carries,
  only when a dependency's serial moved (or the declaration is new or invalidated). A new extent is a size wish on the twin, so the
  step 4 path makes the version on the lane (born cleared for History). A new client key registers the client there and the revision
  waits for every stage build that holds it (`FrameAnchors::StageBuildNeed`, checked in `AdvanceRevisionRecordings`). So the values,
  the versions and the client switch in at one Begin.
- `CS::Setting` variables now carry the value (`Vars::Lane::SettingValue`, up to 32 bytes); the raise hook passes the bytes.
- Exponential Height Fog: twelve textures declared at Setup on `DisplaySize` (decision 6a: the grid covers the display, the shaders
  already map the dynamic-resolution depth region), ViewAtmosphere's `nearPixelSize`, `nearSlices`, `middlePixelSize`,
  `middleSlices`, and EHF's `enabled`, `volumetricFogEnabled`, `volumetricFogExtinctionScale`, `fogDensity`, `fogDensity2`. They are one
  texel while the volumes are off and are published once. The client is declared on the toggles and pixel sizes; its key adds the
  inputs the last Prepass found. Constants, group counts and `Slices()` read `Adopted().Extent`; history validity reads `FirstFrame`.
  `FollowSizes`, `ResetHistories`, `Build`/`Register` from the Prepass, the per-frame release and debug mode 10 are gone.
- Worker rv61a split the GPU-copied structs (`ExponentialHeightFog::GpuSettings`, `ViewAtmosphere::GpuSettings`) and ported the fields;
  EHF's `ProjectSizeSettings` is gone and ViewAtmosphere's keeps `enabled`, `distantPixelSize`, `distantSlices`.

**As built (2026-11-19, 6.2, `org-native/rev-step6-2`).** ImGui's atlas pages publish through the revision.
- `Frame::Revision::PublishTexture(slot, image, owner)` (any thread) is a new revision input beside `PublishBuffer`: a texture the
  caller made becomes a pending version of a `VersionedTexture`. The lane adds it to the revision's `MakeTextureVersions` call as an
  image request with undefined contents, so it lands in the same `NamedVersions` set and fragment as the twins' versions. Its slot
  joins the closure, the member stages record against it, and `AdoptAll` makes it current with the rest. Latest wins per slot. A
  superseded revision gives its textures back unless newer ones came. The adoption line lists them under twins as
  `name (published)`.
- `ImGuiBasicRHI.cpp:SetPage` calls it for the page's texture or its placeholder. `PublishPages`, `nextPages`, `publishQueued` and the
  `AtNextBegin` call are gone. The upload and draw passes still skip work on a texture their page does not bind, and the whole
  texture is restaged until a recorded upload copied its bound version, so nothing else changed.

**As built (2026-11-19, 6.8, `org-native/rev-step6-8`).** LLF's cluster buffers keep their Setup size, the display size's grid,
which `ORGLightCulling::Setup` now takes as the capacity. The passes dispatch for that capacity, and the push constants carry
only it. The run's grid travels in the latch (`LLFLatch::clusterSize`); the shaders clamp it to the capacity and skip clusters
outside it. The light copy always moves the whole capacity. So no pass revision follows the grid or the light count, and
`pending`, `BeginFrame` and the atomic shape are gone. `LightLimitFix::UpdateClusterParameters` takes the grid from
`ORGLightCulling::AdoptedClusterGrid` (adopted `DisplaySize` times adopted `DynamicRatios`, 64 pixels a cluster) while the graph
culls, so the culling and the frame's readers (`PerFrame::ClusterSize`) use one grid. Plain D3D11 keeps the live formula.

**As built (2026-11-19, 6.13, `org-native/rev-step6-13`).** LLF's local shadow cache follows the revisions.
- `EnableLocalShadows`, `LocalShadowSlots` and `LocalShadowResolution` are `CS::Setting`s. LLF's `ProjectSizeSettings` is gone.
- `ORGLocalShadows::Setup` runs once, at `SetupResources`, when the local shadow hooks are installed. It declares the cache with
  `DeclareTexture2D` on `EnableLocalShadows`, `LocalShadowResolution` and `EngineShadowMap`: a new resolution is a new version, and
  the cache is one texel while it is off. The client is `DeclareClient` on `EnableLocalShadows`, `LocalShadowSlots` and
  `EngineShadowMap`, and its key is the slot count. There is no key while the cache is off, or the engine has no shadow map array.
- **A new slot count is a new slice count, so another shape class.** The client's make declares a new cache texture for that
  count. A new client registration then waits for its EarlyPrepass stage build inside the revision. The revision that adopts it
  publishes the new texture under `LightLimitFix::LocalShadowCache`. That is one structural rebuild, at that boundary, for the
  readers that declare the id. Each shape class's texture has a graph name of its own (`... (N slots)`), so it never meets the
  published id. Going back to the adopted count takes the adopted texture again.
- `Frame::DeclareClient` takes an optional fifth argument, `adopted(key)`. It is called inside the adoption point (at
  `Declarations::NoteBegin`), once per registration, at the first Begin whose adopted values give the registered key. LLF uses it to
  switch the cache it binds at t103 and publishes. Additive: EHF and the other callers pass nothing.
- The slot data buffer holds every slot the setting allows (64 times 112 bytes), so a slot count changes nothing there. No buffer
  declaration was needed. The data copy and the latch always move the whole capacity, with the slots past the adopted count zeroed.
- The frame reads the cache's shape only from `ORGLocalShadows::Adopted()`: the adopted texture's slots, its bound extent and the
  adopted `EngineShadowMap`. `LightLimitFix::FollowLocalShadowGraph` makes the CPU side follow it and forgets the slice
  assignments when it changes. The copy constants and group counts are written per run from it.
- Gone: the mid-frame `EnsureLocalShadowResources` on the graph path, `ORGLocalShadows::Release`, `Desc`, Setup's same-shape
  guard (`SameShape`), `WithdrawCache`, `localShadowGraphFailed` and the move to D3D11 when the graph lost the cache. Plain D3D11
  keeps `EnsureLocalShadowResources` as it was.
- `CS_LLF_LOCAL_SHADOWS=1` (tests) turns local shadows on at PostPostLoad, so their hooks install. The config is
  `llf-local-shadows`.

**As built (2026-11-19, 6.16, `org-native/rev-step6-16`).** IBL's client is declared once, at Setup, on `ReflectionCubemap` and
IBL's `DALCMode`. Its key holds whether the reflection cubemap exists, whether the DALC mode reads the environment SH (modes 0
and 1) and whether Dynamic Cubemaps publishes its cubemap (read at evaluation: it is published once, at Dynamic Cubemaps' setup).
`make` takes the sources from the key. `Refresh`, `declared` and the per-frame source compare in `ORGIBL::Prepass` are gone; the
scene switch (`IsDisabledForCurrentScene`) stays the client's `active`, a per-frame gate. `EnableIBL`, `UseStaticIBL` and
`DALCMode` are `CS::Setting<std::uint32_t>`, bound in `IBL::IBL()`; IBL's `ProjectSizeSettings` is gone. Exponential Height Fog's
key now holds IBL's SH from `EnableIBL` (a dependency) and IBL being on the graph, and Skylighting's probes from Skylighting being
on the graph. The scene switch is a constant (`ConstantInputs::ibl`), so the fog's client no longer changes at loading screens or
the map. Test: `ibl` (new).

**As built (2026-11-19, 6.17, `org-native/rev-step6-16`).** Skylighting's five probe volumes are declared at Setup on a reset count,
`Skylighting::resets` (a `CS::Setting<std::uint32_t>` named `Skylighting.resets`, not saved). `ORGSkylighting::QueueReset` raises it.
Declare.h gained `RenewFn`, an optional last argument of `DeclareTexture2D/3D`: a new count makes a new version at the same
extent. The revision's wish path carries it (`RevisionWish::renew`, a serial, so a second reset while one prepares is not lost).
A new version is born zero, which is not Skylighting's reset state (unit SH, lit history bits, full visibility, unknown coverage),
so the five clears stay as gated passes (run gate entry 3, recorded at execution) that the commit opens on the first run after
`FirstFrame` of a volume or a new adopted count. The client is declared on its imports (the key, set by `Update`, `Invalidate` on a
change: an interim, as EHF's inputs). The one-shot clears, `QueueReset` → `Rearm` and `Refresh` are gone, so SSGI is the last
`Rearm` user. The zenith copy stays a gate. `FrameAnchors::Scheduled` in `ZenithReady` and the guest map clear became "a commit
of the client ran". Test: `skylighting-reset` (new).

**As built (2026-11-19, 6.14, `org-native/rev-step6-14`).** Upscaling's textures and clients follow variables.
- `upscaleMethod`, `upscaleMethodNoDLSS`, `qualityMode`, `frameGeneration`, `frameGenMethod`, `frameGenMultiplier` are
  `CS::Setting`s, bound in the new `Upscaling::Upscaling()`. Upscaling's `ProjectSizeSettings` and its `SettingsWritten` calls are
  gone. The test requests write the settings in `BeginRenderFrame` (the `AtNextBegin` calls are gone), and the revision that
  carries them switches the method in.
- The frame runs `Upscaling::FrameUpscaleMethod()` and `FrameQualityMode()`: the adopted `UpscaleMethod` and `qualityMode` with the
  render graph, the live values in plain D3D11. `ConfigureUpscaling` (render size, ratios, jitter), `ConfigureTAA`, `Upscale`,
  `IsUpscalingActive`, `Main_PostProcessing`, `PostChain.cpp`'s `post.upscaler` and `State.cpp`'s mip bias read them.
  `UpscaleMethod` is still written from `GetUpscaleMethod()`, the live choice the next revision carries.
- `ORGUpscaling::Setup` declares the upscaled texture on `DisplaySize`, `UpscaleMethod` and `frameGeneration`: display size while
  an SDK upscaler (or guest DLSS-G's evaluate) writes it, one texel otherwise. So it now follows a size change. The hudless colour
  has three textures, one per format of `ORGUpscaling::kHudlessFormats`, declared on `DisplaySize`, `HdrEnabled`, the new frame
  variable `PresenterFormat` and `frameGeneration`. The one `HudlessFormatIndex` picks is display size, the others one texel. So
  an HDR toggle changes extents only, never a format. `PresenterFormat` (`Vars::SurfaceFormat`, DXGI format and HDR10) is written
  every frame from `Upscaling::BeginRenderFrame`, skipping frames with no committed format.
- The Upscale client (copy into kMAIN and the depth upscale) is declared on `DisplaySize`, `UpscaleMethod` and `qualityMode`; its
  key holds the copy's render area and whether the depth upscale runs (SDK method below native). Its make also registers the
  evaluate client. The hudless client is declared on the hudless textures' variables; its key holds the format. Passes declare
  the textures through their slots. `RegisterUpscale`, `RegisterHudless`, `UpscaleChanged`, `HudlessChanged`,
  `CreateTexture`/`DestroyTexture`/`Desc` and RunUpscale's registration are gone.
- `CheckResources`, `previousUpscaleMode`, `previousUpscalingWasActive`, `DestroyUpscaledTexture`, `CreateHudlessTexture` and
  `DestroyHudlessTexture` are gone, and with them the two resize `DrainCommandRing` calls. Plain D3D11 makes its upscaled
  texture and one hudless texture per format on first need and keeps them. The interop ring already holds every D3D11
  resource its submissions use until their slot completes (`QueueResourcesForDeferredRelease`).
- `Streamline::EvaluateDLSS/XeSS/FSR` set their SDK options where the evaluate records: inside the evaluate pass's record in graph
  modes, just before the ring's evaluate in plain D3D11. The options carry the frame's quality, output size and pre-exposure
  (native frame generation plan, decision 10a).
- Interims left: engine imports (kMAIN, depth, the copies, the hudless source) are still found by the frame. A change bumps a
  serial in the client's key and calls `DeclaredClient::Invalidate` (step 9b). The `FrameAnchors::Scheduled` gates stay on the
  evaluate's copy, the hudless copy and the frame generation inputs. See "Decided by the agent (step 6.14)" in section 9.

**As built (2026-11-19, 6.21, `org-native/rev-step6-21`).** Atmosphere and Fog's medium and post-water composite follow the
revisions.
- `enabled`, `distantPixelSize` and `distantSlices` are `CS::Setting`s too, so all seven size and toggle fields are. ViewAtmosphere's
  `ProjectSizeSettings` is gone. The atlas layout is one function, `ViewAtmosphere::LayoutFor`, which D3D11's `Prepare` and the
  graph's declaration share.
- The luminance and transmittance atlases are `DeclareTexture3D` on `DisplaySize` and the seven fields (at most 8 M texels),
  published once at Setup. While the shared volume is off they are one texel (the existence rule). It is always off on the graph,
  since its inject and integrate passes wait on Physical Sky's media (`view-atmosphere-port.md` 3.6), so no setting change makes
  a version yet.
- Both clients are declared at Setup. The medium has no dependencies and registers at the first revision, no longer from the first
  Prepass. The composite is `DeclareClient` on EHF's `enabled`. Its key is what the last Prepass observed: Physical Sky off, the
  program built (the newest build of `ProgramCache`), the inputs published, and kMAIN and its depth. These are not variables yet,
  so a change calls `Invalidate()`, as EHF does with its inputs.
- `CompositeActive()`, and so `PostWaterComposite`, is the registration the adopted revision holds: the client's `adopted` call
  sets it, so the composite and the flag switch in at one Begin. A removal (fog off, Physical Sky on) stops both at once: the gate
  closes when the client is unregistered, and the shaders do their own fog from that frame on.
- Every composite registration sets `matchesById`, keeps the gate and commits the same run, so a registration for new inputs lets
  the older one compose until its revision is adopted. No frame has the flag on without the composite.
- Gone: the `FrameAnchors::Scheduled` gate in `CompositeAfterWater`, `CompositeActive()`'s `ScheduledThisFrame` (Q12's point fix),
  the in-frame registration of both clients (`UpdateComposite`, `RegisterComposite`, `RegisterMedium`, `compositeWanted`).
- The shared volume's switch (`SharedVolumeActive`, still false) uses the existence rule when it lands: the fog's volumes go one
  texel through their declaration, and their ids stay published. `view-atmosphere-port.md` 3.3 says so now.
- The config is `view-atmosphere`.

**As built (2026-11-19, 6.6, `org-native/rev-step6-6`).** Screen Space GI's textures, programs and client follow the revisions.
- `Enabled`, `EnableGI`, `EnableExperimentalSpecularGI`, `EnableTemporalDenoiser`, `EnableBlur` and `ResolutionMode` are
  `CS::Setting`. The menu's preset buttons call `ScreenSpaceGI::ApplyPreset`, whose writes are `Setting` writes. The
  `ProjectSizeSettings` entry and `recompileFlag` are gone. The D3D11 path recompiles when the settings its defines come from differ
  from the compiled ones (`CompiledDefines`, checked in `DrawSSGI`).
- Ten History textures are declared on `DisplaySize` and `Enabled` (the specular pair also on the specular and temporal
  settings), one texel while they do not exist: the accumulation count, the previous geometry, and two textures per composite input.
  The four outputs are exported and published once at Setup under the composite's ids; their partners are not exported. At full
  resolution an output is also the history and its partner the scratch; below it the partner is the history and the upsample
  writes the output. So the resolution mode swaps roles in the client and makes no texture change. The working textures (depth,
  normals, radiance, accumulation scratch, blur inputs) are transients made at each registration at the key's display size.
- One `DeclareProgram` (`ScreenSpaceGI::Chain`) on the define settings builds the seven step programs for a define set. Every step's
  defines with the common ones are resolved at Setup (`ResolveDefines`), so a build reads no State.
- The client `cs.ssgi` is declared on `DisplaySize` and the six settings; its key adds the engine inputs the last Prepass found
  (`Invalidate` on a change). While SSGI is off, or before the inputs are found, the client holds only the history clears.
- `cs.ssgi.reset`, `RegisterReset`, `RegisterChain`, `Rearm`, `PublishOutputs`, `RequestSize`, the program prebuild and the D3D11
  fallback after Setup are gone. A history reset is a clear of every declared texture behind one gate entry, which the commit opens
  on a loading screen or shader reload (`queuedResetHistory`), on `FirstFrame` of any declared texture, and on the first run of a
  registration made for another configuration or size. Dispatches, `TexDim` and the clamp read `Adopted().Extent`.
- The composite always gets the published outputs while the graph path runs (zero while off), and takes the specular texture by
  the adopted `EnableExperimentalSpecularGI` (`ScreenSpaceGI::FrameSpecularGI`).
- cs-test: `ssgi` config and the feature's `ssgi-preset` command. EHF's client key reads no SSGI state.

---

### Step 7. Delete the old paths and make the check fatal

**7a. Deletions.** `RequestSize`, `RequestExtent`, whatever is left of `ApplySizeRequests`, `ResizeGeneration`, `BumpResizeGeneration`,
`ResizeCauses`, `FrameAnchors.cpp:Service`'s resize block, `PublishedResources::Refresh` (its only caller was `Service`),
`Frame::Resources`' `fresh`, `FrameAnchors::Rearm` and the one-shot (`once`/`armed`) machinery if Skylighting's reset became a variable
in step 6, `AtNextBegin` (no users after Terrain Shadows and ImGui migrate), `Feature::SettingsWritten` and
`ProjectSizeSettings` (all fields are `Setting<T>`), the `RequestSize` input of the lane. `GraphResources.h`'s header rules rewritten
for revisions.

**7b. Guest own-epoch anchors.** HDROutput and FrameGeneration's hudless copy run on their anchors' own tickets and can take a build
mid-frame (`FrameDriver.cpp:OnBegin` comment; cleanup plan piece A7). Make them staged members of revisions (or CS stage steps if A7
lands first; check `docs/development/org-native-frame-graph.md` for A7's state). Then delete `Builds::SwitchStaged` and
`BuildManager::SwitchStaged`.

**7c. Shader reload.** `RenderGraphRuntime::RequestShaderReload` → `ServicePipelineReload` → `Builds::RequestAllSynchronous` becomes an
event (`Vars::ShaderGeneration`) whose revision has every epoch as a member, built async and staged. The pipeline swap that needs the host
thread held happens inside `AdoptLatest` **(not investigated; open question Q5)**. If the swap cannot be made safe that way, keep
`RequestAllSynchronous` for the pipeline swap only and record why in the dated doc.

**7d. Fatal.** The boundary check's late versions and late attaches become `NoteTapError` (a new `TapError::OutsideAdoption`), fatal
in development builds. Allowed callers left: `dclf` (until 10a), the engine-import exception (until 9). Late changes (in-frame build
requests) stay counted until step 10c.

**Test.** Full set: standard set, `resize`, `resize-drag`, `menu`, `hdr`, `fg` (guest and `d3d11+fg`), `guest+map`,
`latch-census-short`, `upscalers` with `native-vulkan`.

**Acceptance.** The section 8 greps for step 7 are empty. No tap error in the full set.

**Size.** Medium. 7a one agent; 7b and 7c separate agents (disjoint files), after 7a.

---

### Step 8. Dynamic resolution as viewports (closing)

Most of decision 6a lands inside step 6. This step does what no feature migration owns:
- Frame-wide sites: `Frame/PostChain.cpp:Read` (`post.dynres` in the build key and the frame description), ImageSpace chain viewports
  baked at registration (`ChainSteps.inl:RegisterChainSteps`), Upscaling's input viewports. A ratio change becomes a constant and a
  viewport in the latch, not a key build.
- Enforcement: flip `VariableTraits::sizes = false` for `RenderSize` and `DynamicRatios` to a hard registration error (it was a logged
  error during step 6).
- Measurement: VRAM at 4K with an upscaler at its lowest ratio, before and after (the cost of display-size textures). Record it in the
  dated doc. Use the VRAM census (`stats-window` or `vram-all` with `native-vulkan`).

**Test.** `upscalers` in three modes, `flicker-taa` with `native-vulkan` (TAA history at display size), new `dynres` config: quality
mode changes through the `setting` step (`Upscaling.qualityMode`) with shots. Acceptance: no adoption at all for a ratio change (only
constants), no key build, shots match.

**Size.** Medium. One agent.

---

### Step 9. Engine imports through events

**Purpose.** Decision 3a. Announced engine changes are events. A discovered mid-frame import change is fatal.

**9a. Announced changes.**

> **Decision (2026-11-18):** a mid-session `Create` of the engine's targets stays a fatal error. Do not build the event path for
> it described below. Build 9a only for the changes that do occur: the swap chain's `ResizeBuffers` and colour-space changes. See
> section 9.
- `EngineTargets.cpp:EndSet` raises `DisplaySize` and an `EngineTargets` serial. The new images are made by the engine's call (they
  cannot be made ahead, correction 4). `Move`/`AdoptSlot` change from `Publish` to `Make`: the new versions are pending and join the next
  revision. Delete `Settle`'s `BumpGeneration` and `Builds::RequestAll`; the revision rebuilds what depends on the shape class.
- Native back buffer: `NativeDevice.cpp` resize path raises `DisplaySize`; the back buffer's next version (published "after the frame's
  tail" today) becomes a pending version of the revision.
- HDR swap chain colour space and format: an `HdrEnabled`/`SwapChainFormat` revision. The old chain draws until the new one is adopted,
  which removes the "few black frames" on an HDR switch (`parity/status.md`).
- **Open question Q4** decides whether the engine reads its new D3D11 objects between `EndSet` and the next Begin. If it does, the
  revision must be adopted before the engine's next draw: `EndSet` would then be the boundary itself. Resolve before coding 9a.

**9b. Discovered imports.** `FrameAnchors.cpp:JoinSlot` and `EnsureViews` outside the adoption point become `TapError::OutsideAdoption`
naming the import and the caller. Work through each source the step 1 census listed: an import whose texture follows a variable gets a
declared dependency instead (ImageSpace `CheckImports`, Upscaling's `QueueEvaluate`/`QueueCopyToMain`/`QueueDepthUpscale`/`RunUpscale`/
`CopyHudless`, HDR `Compose`, `BinkGraph.cpp:UpdateTargets`, `Menu/ImGuiRenderer.cpp:UpdateTarget`). `EnsureViews` (more views on an
import) becomes "declare the views at registration". Sources added since: the cubemap capture imports kMAIN until Dynamic
Cubemaps' first update and Deferred's forward main target after it (`110daff05`), so its import switches once in a session;
the fog takes Deferred's frame-checked directional shadow map (`07608f8e8`). Each becomes a declared dependency (the forward
target as the import from the start, with Dynamic Cubemaps' "first update done" as a variable if the two really differ).
- Delete the version-lag branch in `Schedule` (the step 4 intermediate) once its census count is zero by construction.

**Test.** Standard set, `hdr` in four modes (acceptance: no black frame in a shot taken 2 frames after the switch, a new `shot` step at
`frames = 2`), `menu`, `fg`, `guest+map`, `latch-census-short`. A window-resize test is not possible today (correction 4).

**Acceptance.** The boundary census shows zero engine-import late versions. `hdr` shows no black frame.

**Size.** Medium to large. 9a and 9b separate agents; 9b's sources can split by feature.

---

### Step 10. DCLF joins

**10a. Stage DCLF's epochs.** `RenderGraphRuntime.cpp` (the `BuildManager::Desc` setup): DCLF epochs move from `external` to `staged`.
- `MakeRevision` (`Producer.cpp`) records on the **staged** generation instead of returning early while the build state is newer than the
  attached one. The snapshot carries the staged generation.
- `AdoptSnapshot` is called from `AdoptLatest` (both modes; in guest this moves DCLF's adoption from `Main::Draw` to `Renderer::Begin`,
  open question Q7). A snapshot for a staged build is adopted together with `AttachStagedEpochGraphs` of DCLF's epochs.
- Delete `AdoptCurrentSnapshot`'s wait, `RenderGraphRuntime::AttachDclfBuilds`, the frame-end `Poll(..., Attach::All)` for DCLF, and
  the `dclf` allowed caller of the boundary check.
- `kStaleTargets`, `kStaleShadowFormat`, `kStaleMain` (correction 5): these generations become variables (step 9 makes target changes
  events), so a snapshot made for them is adopted with the change that caused them. If any of the three can still change outside a
  revision at this point, 10a stops and that source is moved first.
- **Intermediate:** DCLF's snapshot is still its own `LatestSlot`, adopted in the same call. Deleted in 10b.

**10b. The snapshot as a fragment.** DCLF's `RevisionAssembler` sealing moves into the frame revision: `kScene` (publication, draws,
payload) and DCLF's buffer `NamedVersions` become fragments of the frame assembler.
- DCLF recordings name `ComposedVersions{DCLF buffers, frame textures}`, so a CS texture change has DCLF epochs in its closure and they
  are recorded against the pending version (closes correction 2).
- The scene cadence must not hold a resize back, nor the reverse: fragments inherit, and a revision with only `kScene` changed seals as
  soon as its recordings are ready (risk, section 7.2).
- Delete `snapshotSlot` (`LatestSlot` of snapshots), `Impl::StaleKindOf`'s build and generation checks, `AdoptSnapshot` as a separate
  adoption (its body becomes the `kScene` install callback).
- Ports' DCLF data, and where each lands:
  - Physical Sky's and ViewAtmosphere's frame textures read published ids, so a new version of a LUT or of the medium has DCLF's
    colour and face epochs in its closure through `ComposedVersions`, like any CS texture. The guest-only captured D3D11 views
    (AP shadow, clouds) stay late-bound until those resources are graph twins.
  - ViewAtmosphere's reflection medium through per-epoch views (decided): the view a frame texture names per epoch is part of
    the bound set of that epoch's recording, so it changes only with a revision. It is not a late-bound per-frame switch
    **(inferred: the per-epoch frame-texture API is not built yet; build it as bound)**.
  - Advanced Skin's material table is a frame-value pool: late-bound, outside revisions, nothing to move. The frame binding
    count (6 since t38) is bound and set at setup. Its `GpuTextures` generation re-put is DCLF's own event, not a frame variable.
  - Tree Wind for object LOD (deferred, redesign pending): its `Draws::Versioned` buffer grows with the scene buffers, so it is
    one more entry of DCLF's buffer `NamedVersions`, adopted with `kScene`. Its commit-hook uploads tag each region, which keeps
    a record from reading a region the adopted version does not hold yet. If the redesign keeps the CPU copy path, its pool jobs
    stay DCLF's scene-lane work, never revision work.

**10c. Toggles and conditions as variables.** `Toggles` (`ToggleSet` fields) and `GraphConditions` become variables in `kInputs`.
Delete `Toggles::BeginFrame`/`Adopt` and `RefreshGraphConditions`' polling at the build point. Then make in-frame build requests
(step 1's late changes) fatal too. Tree Wind's `TreeModSupport` (when its port lands) changes the classification of DCLF members,
not a graph or a bound resource: its `Setting<T>` event wakes DCLF's scene lane to re-classify, and the result arrives with a
later `kScene` fragment.

**Test.** Everything in step 7's list plus `native-census`, `native-tour` (long), `river` with `native-vulkan`, `rain` with `guest`,
`flicker-taa`. DCLF-specific: the DCLF settings menu toggles (cull mode, CPU/GPU pipeline) through `setting` steps in a new
`dclf-toggles` config.

**Acceptance.** No `[DCLF] frame N has waited` lines, no `CS.DCLF.WaitCurrentSnapshot` zone in a Tracy capture of `native-tour`.
"Declared view mismatch" gone after a `resize` run. Section 8 greps for step 10 empty.

**Risks.** DCLF is the most tested and most complex part. Keep 10a, 10b, 10c as separate commits with full runs between them. Detect
regressions with `latch-census-short` and `native-tour` shots against the step 9 baseline.

**Size.** Large. Serial, one agent per sub-step.

---

## 5. The feature migration list (step 6)

Order: easiest and best tested first. "Polls" is what it checks inside the frame today. Sizes after decision 6a: render size becomes
display size wherever a texture followed it.

| # | Feature / site | Variables | Polls today (where) | Re-creates today | Target declaration | Special handling |
|---|---|---|---|---|---|---|
| 6.1 | Exponential Height Fog | `DisplaySize` (was dynamic screen), `volumetricGrid{,Far}PixelSize`, `volumetricGrid{,Far}SizeZ`, `enabled`, `volumetricFogEnabled`, `volumetricFogExtinctionScale`, `fogDensity>0`, IBL/DynCubemaps inputs | `ORGExponentialHeightFog.cpp:Prepass` → `Impl::FollowSizes` → `VolumeTextures::RequestSize`; `ExponentialHeightFog.cpp:ComputeGridSizes`; `ExponentialHeightFog::Prepass` releases every frame while off; D3D11 `EnsureVolumetricResources` | 8 Texture3D + 4 Texture2D versioned twins; `Build` → `Register` on `Shape` change; re-enable publishes new twins under the same ids (rebuild every graph) | 12 `DeclareTexture3D/2D` on display size and grid settings; `DeclareClient` on `Shape` (groupReduce, far, temporal, inputs); disabled = 1-texel versions | History → `FirstFrame`, delete `ResetHistories`. Constants and `Slices()` (read by `ExponentialHeightFog::GetSettings` for the D3D11 CB, and by readers through shared data) from `Adopted()`. GPU-copied `Settings` struct: split CPU settings from the GPU struct (section 6.1). The D3D11 fallback (`CS_ORG=0`) keeps its own path, untouched. Has `resize`. |
| 6.2 | ImGui atlas pages | page texture events | none (event); `ImGuiBasicRHI.cpp:SetPage/InvalidateTextures` → `AtNextBegin(PublishPages)` | versions of a page slot | page versions as pending versions of the next revision | Removes one `AtNextBegin` user. `menu` config. |
| 6.3 | Terrain Shadows height map | worldspace height map (event) | none; `ORGTerrainShadows.cpp:SetHeightmap` → `AtNextBegin` → `Switch` (direct `Resize`) | new persistent twin, staging, new client | `HeightMap` variable (identity + size), texture declared on it, client on it | Removes the other `AtNextBegin` user. Test: `coc` between worldspaces (add to a config). |
| 6.4 | Screen Space Shadows | kMAIN size via `DisplaySize`, `SampleCount` × dynamic area, Terrain Blending's depth source | `ORGScreenSpaceShadows.cpp:Prepass` (`ShadowMaskSize`, `GetScaledSampleCount`, `CurrentDepth`) | versioned exported output via `RequestSize`; program per sample count; `Refresh` | output on `DisplaySize`; program on `SampleCount` and `DynamicRatios` (derived count); client on Terrain Blending's `Enabled` | Dispatches from `Adopted()`. Uncommon counts built synchronously today: make them program-variable builds on the lane. |
| 6.5 | Terrain Blending | `DisplaySize` (kMAIN), `Enabled` | `ORGTerrainBlending.cpp:BeforeDepth` | 3 versioned exported depths via `RequestSize`; `RegisterClient` again for group counts | 3 textures on `DisplaySize`; client on `Enabled` | `MakeTerrainDepth` (setup-only, published) declared on `DisplaySize` too. GPU-copied settings: split. |
| 6.6 | Screen Space GI | `DisplaySize` (kMAIN), `ResolutionMode`, `Enabled`, `EnableGI`, `EnableExperimentalSpecularGI`, `EnableTemporalDenoiser`, `EnableBlur`, debug overrides | `ORGScreenSpaceGI.cpp:Prepass` (`Config::Current`, `recompileFlag`, `MainDesc`) | same-shape kept, else new twins; `RegisterReset` + `RegisterChain`; `PublishOutputs` of new objects (rebuild every graph); `Rearm(kResetClientId)` | textures on `DisplaySize` + `ResolutionMode`; outputs keep one slot each (1-texel when off); programs on the define fields; client on config | `Rearm` → `FirstFrame`. Dispatch clamp from `Adopted()`. Quality preset buttons in `DrawSettings` assign fields: they become `Setting` writes. |
| 6.7 | ImageSpace kept textures and chain | `DisplaySize`, final target (HDR's `PostChainTarget`), luminance levels, SAO levels, water copy widths, flow map | `ImageSpace.cpp:BeginFrame` (first ImageSpace call): `CheckImports`, `CheckChainSteps`, `CheckWorldSteps`, `CheckStartSteps`, `CheckMenuSteps`, `CheckFlowMap` | `Frame::Resources::Texture2D` (`RequestSize`, `fresh`, new twins); re-register 3 chain clients; publish adapt pair and flow map (rebuild every graph) | kept textures declared on `DisplaySize`; chain clients on `HdrOutputTarget`, levels; adapt pair keeps its slots | Viewports from `DynamicRatios` move to step 8. Delete the retry every 120 frames. `flicker-taa`, `hdr`, `guest+map`. |
| 6.8 | LLF clusters | `DynamicRatios` (cluster grid = dynamic screen / 64) | `LightLimitFix.cpp:UpdateClusterParameters` → `ORGLightCulling.cpp:Execute`; `pending` shape applied in `ORGLightCulling::BeginFrame` | pass revision, recording | cluster buffers sized for `DisplaySize` (they already use the non-dynamic size at `SetupResources`); grid count from `Adopted()` as a constant | Delete `pending`. No resource change: the pass reads the count from constants. |
| 6.9 | Post effects | effect list and order (`enabled` per effect), CODBloom used mips (derived from `MipBlendFactor`), DoF (`UseAdaptiveGather`, `GatherQuality`, `BokehMode`, `HighlightShape`), PhysicalGlare (`FFTResolution`, `ApertureMode`), LensFlare (`FFTResolution`, `GhostModeInt`), LUT (`LutPath`), ColorGrading tonemapper, kMAIN size and format | `ORGPostProcessing.cpp:BeforeTonemap` (`key`, `Impl::Rebuild`); per-effect `Update`/`Build` | new transient twins, `Register`; kept twins in effect `Build` (MotionBlur `horizontal`, PhysicalGlare, DoF, LUT, LocalExposure, ColorGrading LUT, LensFlare spectra, HistogramAutoExposure buffers) | per-effect `DeclareClient` on its fields; kept textures declared; `dynamicWidth` out of the key (step 8 for the viewport) | HistogramAutoExposure `clearPending` → `FirstFrame`. Derived variable for CODBloom so a scene-layer blend of floats does not rebuild unless the mip count changes. Needs the nested `setting` step (5). Split by effect across agents. Physical Lens and Camera FX are 6.20. DoF publishes its focus to the chain (`Published::kDoFFocus`), so the lens declares it. HistogramAutoExposure's readback now feeds the pre-exposure (`State::UpdateSharedData`), which stays a per-frame late-bound value. |
| 6.10 | Sun Rays (Effects11) | `DisplaySize` (half sizes were dynamic), kMAIN format, `EnableSunRays`, inputs | `ORGSunRays.cpp:PrepareFrame` | 5 transient twins, programs, `Register` | textures on `DisplaySize`; client on inputs | `Effects11::ClearShaderCache` → `Release`: a program variable. |
| 6.11 | Effects11 kept textures | display size (`screenWidth/Height`), technique enable bits, tonemap owner | `ORGFxRuntime.cpp:Prepare` | new `Resources` set with one-shot clears; `Planner::Build` + `Register` | kept textures on `DisplaySize`; client on `ShapeKey` | one-shot clears → `FirstFrame`. Effects11's own `SettingManager` (string keys): its writes raise the shape variable at its setter. |
| 6.12 | Volumetric Lighting | interior/exterior quality and custom size, engine VL descriptor, `DynamicRatios` (blur area, latch only) | `ORGVolumetricLighting.cpp:EndRender/Blur` (`DescriptorSize`, `SettingsSizes`) | `RegisterAgain`: new transient Texture3D; frame skipped until scheduled | textures on the size settings and an `Interior` variable; blur area stays a constant | Delete the skipped frame (the old revision draws until adoption). `guest-interior`/`native-interior`. |
| 6.13 | LLF local shadows | `LocalShadowResolution`, `LocalShadowSlots`, `EnableLocalShadows`, `EngineShadowMap` (size, format, slices) | `LightLimitFix.cpp:EnsureLocalShadowResources` (mid-frame) | `ORGLocalShadows::Release` + `Setup` (new exported twins, publish replaced: rebuild every graph) | cache array and data buffer declared on those variables, same slots | A new slice count is another shape class: structural, prepared in the revision. Delete Setup's dead same-shape guard. |
| 6.14 | Upscaling | `UpscaleMethod`, `qualityMode` (→ `RenderSize`), `frameGeneration`, `frameGenMethod`, `frameGenMultiplier`, HDR and presenter format for hudless | `Upscaling.cpp:CheckResources` (static `previousUpscaleMode`), `CaptureHudlessColor` → `CreateHudlessTexture` every frame; `ORGUpscaling.cpp:QueueEvaluate/...` re-register | `DrainCommandRing`, destroy/create upscaled and hudless twins; `UpscaleChanged`/`HudlessChanged`; `FrameGen::Controller::Reconcile` | upscaled texture on `DisplaySize` + `UpscaleMethod` (today it never follows a size change: a gap); hudless on `HdrEnabled` + presenter format; client on method | Delete every resize `DrainCommandRing` and `previousUpscaleMode`. Test requests (`RequestUpscaleTest`/`RequestFrameGenTest`) write `Setting`s. `upscalers`, `fg`. |
| 6.15 | HDR Display and HDR output | `HdrEnabled`, `HdrOutputTarget`, back buffer identity, program | `HDRDisplay.cpp:BeginRenderFrame`; `ORGHDRDisplay.cpp:Compose` per present | re-register HDROutput client (own epoch, mid-frame) | composite client on those variables; HDR textures on `DisplaySize` (setup-only today, never resized: a gap) | Depends on 7b for the own epoch. `hdr`. |
| 6.16 | IBL | `EnableIBL`, `UseStaticIBL`, reflection cubemap, Dynamic Cubemaps' env published | `ORGIBL.cpp:Prepass` | `Refresh`, `declared` | client on those variables | — |
| 6.17 | Skylighting | reset event, occlusion/cascade imports | `ORGSkylighting.cpp:ResolveInputs` (from `Update`) → `Refresh`; `QueueReset` → `Rearm` | `Refresh`, one-shot reset | client on imports; `SkylightingReset` variable → `FirstFrame` of its probe textures (a new version, born cleared) | Removes the last `Rearm` user with SSGI. The roof-leak fix (`204f6072f`) adds a published zenith map and a probe coverage volume; the zenith copy runs on zenith frames only, as a gate (late-bound), not a revision. |
| 6.18 | Setup-only features made correct: SSS (`SSMode`, `EnableCharacterLighting`, working textures), Dynamic Cubemaps (`EnabledSSR`, reflection cubemap size), Volumetric Shadows (cascade width), Cloud Shadows, Subsurface Scattering `Refresh` sites | engine target and shadow map variables, their define fields | `AfterDeferredPasses`, `AfterUpdate`, `UpdateImports` (`Refresh`, `programsDirty`) | nothing (sized once) | textures and programs declared on the engine variables and settings | Correct rather than lucky (investigation 3.4 item 5). |
| 6.19 | DCLF family clients' `Refresh` (Sky, DecalOcclusion, BloodSplatter, ShadowMask) and Bink | their targets, `HdrOutputTarget` | `CanTake`/`MakeClient` `Refresh`, `BinkGraph.cpp:UpdateTargets` (+ `BuildPending`) | `Refresh`, a build between frames | clients on target variables | Bink's `BuildPending` goes. These run in CS stages, so they are step 6, not step 10. |
| 6.20 | Physical Lens and Camera FX (post chain, with 6.9) | Lens: `ImageEffects`, `Diffraction`, ghosts on (derived: `Intensity > 0`), the lens model (preset, f-number, focus, zoom, coating: CPU inputs, not variables). Camera FX: its effect `enabled`. Composite: `HAS_PHYSICAL_LENS` while the lens publishes outputs; linear lighting (Composite always draws then, `eb71d2e7f`) | `ORGPhysicalLens.cpp` Commit compares cache inputs each frame (gates, not builds); `StagedBuffer::Poll` each frame; focus readback each camera frame | chain rebuild when Diffraction or ghosts toggle; a new paths version per lens model; Composite's permutation | per-effect `DeclareClient` on those fields and on linear lighting; ghost paths as a `kBuffers` fragment (4e); Camera FX's transient lens image is a frame transient of the chain, no declaration | The caches stay gates (an on-change pass is late-bound work, not a revision). The focus and adaptation readbacks need Q11's answer before the post stage can be a revision member. The ghosts keep the model of the adopted paths version, read from `Adopted()`. |
| 6.21 | ViewAtmosphere (medium and post-water composite) | medium: `enabled`, `{near,middle,distant}{PixelSize,Slices}` (atlas size, at most 8 M texels), `DisplaySize`; composite: EHF on, Physical Sky off (until its media are on the graph), program built, inputs published (`CompositeActive`) | `ViewAtmosphere::Prepare` → `ORGViewAtmosphere::Prepass` (latches parameters each frame); atlas through `RequestSize` (planned); `CompositeActive()` re-registers the composite inside the frame; `CompositeAfterWater` skips frames while `FrameAnchors::Scheduled` is false | versioned Texture3D medium; composite client registered again on an input change | atlas declared on the size settings and `DisplaySize`; composite `DeclareClient` on those variables; `PostWaterComposite` is `Adopted().Get(...)` | Today `PostWaterComposite` (read by Lighting, Effect, Water, Sky and the SSAO composite to skip their own fog) turns on with the registration, while the composite waits for its build: those frames have no fog at all (`view-atmosphere-port.md` open question 4). In a revision both switch together; delete the `Scheduled` gate. The shared volume's switch (`SharedVolumeActive`, planned) withdraws the fog's ids: use the existence rule (keep slots, 1-texel versions) instead. Reflections: per-epoch frame-texture views, bound per epoch (10b). Baselines: see the fog defects above. |
| 6.22 | Physical Sky | `enabled`, `enableVolumetricClouds` (native reports clouds off until phase 4), `halfResApShadow` (use only, not size), `DisplaySize` (AP shadow), Dynamic Cubemaps loaded (AP depth, decision 7 of the port) | LUTs every enabled frame at EarlyPrepass (gated); disk phase LUT only when its 12 constants change (gate); AP shadow `RequestSize` on a resolution change (planned, phase 2) | nothing yet: LUTs are fixed-size History/Persistent twins | LUTs declared at setup (no dependency); AP shadow on `DisplaySize`; `cs.physical-sky.composite` (phase 2, after `cs.deferred`) and `cs.physical-sky` (clouds) `DeclareClient` on `enabled`, clouds and Dynamic Cubemaps | Previous-frame reads (7.12): the reflection faces read the LUTs before this frame writes them. A newly enabled sky's first frame gives the faces born-cleared LUTs (decision 5a: one frame of black sky in reflections) unless the faces take `FirstFrame` and skip. Guest frame textures read captured D3D11 views (late-bound); native gets zero views of the declared kind, so phase 4 switches a register's source from a zero view to a graph resource: give the cloud targets their slots from the start (existence rule). The lightning readback ring (planned) is a readback in the pre-main CS stage: Q11. |
| 6.23 | Screenshot clean scene | kFRAMEBUFFER size and format (`DisplaySize`, `HdrEnabled`/presenter format) | `ORGScreenshot.cpp` compares kFRAMEBUFFER's size and format with its texture on every capture; then `FrameAnchors::Scheduled` | a new Persistent twin and a new client inside the frame; the capture falls back to the D3D11 snapshot until its build switches in | texture and client declared once on `DisplaySize` and the format variable | Moves with the hudless copy in 7b (same FrameGeneration ticket). The capture's own readback buffers are on the Capture anchor's placed epoch, outside the UI stage's recording. |

---

## 6. `Setting<T>` (decision 7a)

### 6.1 Design

```cpp
// src/Utils/Setting.h. No ORG includes: feature headers include it.
namespace CS {
template <class T>
class Setting {
public:
    constexpr Setting(T initial = {}) : value(initial) {}
    Setting(const Setting& other) : value(other.value) {}         // a copy is unbound
    Setting& operator=(const Setting& other) { Set(other.value); return *this; }
    Setting& operator=(const T& v) { Set(v); return *this; }
    operator const T&() const noexcept { return value; }
    const T& Get() const noexcept { return value; }
    void Set(const T& v);              // equality-checked; raises the bound variable's event on a change
    T* Raw() noexcept { return &value; } // for code that must take an address; writes through it are caught by the check (6.3)
    void Bind(SettingVariableId id) noexcept;  // at feature construction, by the feature's registration
private:
    T value;                           // first member: &setting == &setting.value (scene catalog resolver)
    SettingVariableId id{};            // 0: unbound (temporaries, plain D3D11 mode, before the graph exists)
};
}
```

- **Name.** `CS::Setting`. A global `struct Setting` exists in `Features/Effects11/SettingManager.h`; a global template would clash.
- **Event on write.** `Set` compares with `operator==` (floats exactly; a derived variable quantises where needed). On a change and when
  bound, it calls `RenderGraph::Frame::Vars::RaiseSetting(id, value)`, which forwards to the OMS `VariableSet`. In plain D3D11 mode the
  binding stays 0 and nothing is raised.
- **Whole-struct assignment.** `settings = o_json;` (every `LoadSettings`) builds a temporary struct and copy-assigns it. The member's
  `operator=` keeps its binding and raises on a changed value. This covers presets, undo, `LoadSettings`, scene layers, A/B swaps,
  DevBench and cs-test, which all write through `LoadSettings` (section 6.2).
- **Serialisation.** `nlohmann::adl_serializer<CS::Setting<T>>` with `to_json(j, s) { j = s.Get(); }` and `from_json(j, s)
  { s = j.get<T>(); }`. `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT` calls `j.value(key, defaultObj.member)`, which works with the
  serializer. The JSON stays a plain value, so saved settings do not change.
- **ImGui.** `Util::` overloads take `CS::Setting<T>&`: they edit a local copy and call `Set` when the widget reports an edit
  (`Util::SliderInt(label, Setting<int>&, ...)`, `Checkbox`, `Combo`, `SliderFloat`). Raw-pointer casts like `(bool*)&settings.x` on
  ported fields become compile errors, which is how ports find them.
- **Scene catalog generator** (`cmake/generate_scene_settings_catalog.py`): treat `CS::Setting<T>` as `T` in `PRIMITIVE_TYPES` matching.
  The resolver compares `valueAddress` with `&typedFeature->member`; equal because `value` is the first member.
- **Scene widget interceptor** (`CSEditor/SceneManager/SceneWidgetInterceptor.cpp`, `SceneWidgetBinding.cpp:Guard`): it `memcpy`s into
  a widget's value. After that write it calls `CS::SettingRegistry::RaiseAt(address)` (an address → binding table filled by `Bind`), so
  the write raises the event. Permanent: the interceptor is generic.
- **Scene-layer interpolation** blends float fields only and re-runs `LoadSettings` on the whole feature each tick. Each tick is one
  `operator=` per field: only fields whose value changed raise. A size-relevant float (CODBloom's blend factors) feeds a derived variable,
  so a blend raises a size change only when the derived value changes.
- **GPU-copied structs.** EHF, Terrain Blending and the other structs that `FeatureBuffer.cpp:GetFeatureBufferData` or
  `GetCommonBufferData` copy byte for byte cannot hold `Setting<T>` (its layout differs). For those, split: the CPU settings struct holds
  `Setting<T>`; a GPU struct is built from it where the copy happens today. `static_assert(std::is_standard_layout_v<Setting<T>>)` and
  keep `sizeof` checks on GPU structs.
- **Reads in the frame.** For size-relevant fields, the render thread reads `Adopted()`, not `settings.x`. Development builds can flag a
  `Setting::Get` of a size-relevant field on the render thread inside a frame (step 7 adds the flag).

### 6.2 Writers that must raise events in the interim (step 5c)

Each calls `Feature::SettingsWritten(writer)` after it writes (or, for whole-feature writers, the shared function they go through
does). Rows from `Feature.cpp`, `State.cpp`, `Menu/`, `CSEditor/`, `Presets/`, `Testing/`.

| Writer | Where | Interim hook point |
|---|---|---|
| Menu widgets and buttons in `DrawSettings` | `Menu/FeatureListRenderer.cpp:DrawFeatureBody`, `CSEditor/FeatureSettingsWindow.cpp:DrawSettingsPanel` | after `feat->DrawSettings()`, every frame the menu draws it (the projection compare makes it cheap) |
| PostProcessing sub-effects | `Features/PostProcessing.cpp:DrawSettings` | same call (inside the feature's draw) |
| Undo restore | `Menu/FeatureUndo.cpp:MakeTarget` (restore = `LoadSettings`) | `Feature::LoadSettings` wrapper (below) |
| Restore defaults | `FeatureListRenderer.cpp:RestoreFeatureDefaults`, `State::RestoreFeatureLayers` | `RestoreDefaultSettings` wrapper |
| Baseline edits cleared | `State::ClearFeatureEditsUnderBaselines/ClearEditsUnderPack/DropEditsUnder/LoadLayeredSettings` | `LoadSettings` wrapper |
| Preset baselines | `Presets/UnifiedPresetCatalog.cpp` (`EnableBaseline`, `RemoveBaseline`, `MoveBaseline`, `ReapplyIfInUse`), `PostProcessingPresets::WriteBaseline`, `ScenePresetExport.cpp` → `State::ReapplySettingsLayers` | `LoadSettings` wrapper |
| Active pack | `UnifiedPresetCatalog::ApplyPack/DisableActivePack` → scene layer | scene layer row |
| Scene layers and their suspension | `CSEditor/SceneManager/SceneSettingsManager.cpp:ApplyCatalogSceneSettings`, `SceneLayerGuard`, `SceneSettingsResolve.cpp:ApplyResolvedSettings/RestoreAppliedSettings/AdvanceLocationTransitions` | `LoadSettings` wrapper |
| Scene widget interceptor | `SceneWidgetInterceptor.cpp`, `SceneWidgetBinding.cpp:Guard` (`memcpy`) | after the copy |
| Startup and reload | `State::Load`, `Feature::Load` (`Feature.cpp`), Restore Saved (`Utils/UI.cpp`), DevBench `settings load`, A/B start | `LoadSettings`/`RestoreDefaultSettings` wrappers |
| A/B swaps | `ABTesting.cpp` → `State::LoadFromJson` → `Feature::Load` | same |
| cs-test `setting` | `Testing/Commands.cpp:SetSetting` | `LoadSettings` wrapper |
| cs-test `upscale`/`fg` and Upscaling's test requests | `Testing/Commands.cpp` → `Upscaling::RequestUpscaleTest/RequestFrameGenTest`, applied in `Upscaling::BeginRenderFrame` | explicit call after the assignment |
| DevBench `set`/`reset` | `RemoteControl/DevBenchBridge.cpp:BuildFeatureResult` | wrappers |
| Linear lighting mode | `PostProcessingMode.cpp` (`enableLinearLighting = true`) | explicit call |
| PostProcessing pipeline sync | `PostProcessing::SyncPipelineResources`, `pendingSettings` in `LoadSettings` | wrapper |
| Feature self-writes | clamps in `LoadSettings`, `HDRDisplay::SetupResources` auto-detect, `Upscaling.cpp` hardware defaults | wrapper or explicit call |

**The wrappers.** `LoadSettings` and `RestoreDefaultSettings` are virtual and called from many places. Add non-virtual
`Feature::ApplySettings(json, writer)` and `Feature::ApplyDefaults(writer)` that call the virtual and then `SettingsWritten`, and route
the writers above through them. **Intermediate:** both wrappers' `SettingsWritten` calls go in step 7; the wrappers themselves stay only
if they still serve something else, otherwise they are deleted too (checklist).

### 6.3 The development-only missed-writer check

- At `Frame::Driver::OnBegin`, development builds only (not `CS_RELEASE_PROFILE`), outside the stage path: for each feature, compare
  `ProjectSizeSettings()` (interim) or each bound `Setting`'s value (end state) with the value last raised.
- A difference with no event is logged once per field as `[ORG] missed settings writer: <feature>.<field> changed with no event`, and is
  a `TapError` in development runs (cs-test greps tap errors).
- In the end state it catches writes through `Setting::Raw()` and any `memcpy` the interceptor hook missed.
- Not a frame mechanism: it never feeds the revision. It only reports.

### 6.4 Fields to port

Order follows section 5. **S** = size, **Sh** = shape (pass set, program, format, existence), **T** = toggle.

| Order | Feature (struct) | Field : type | Depends on it | Kind |
|---|---|---|---|---|
| 6.1 | ExponentialHeightFog (`Settings`, GPU-copied: split) | `volumetricGridPixelSize`, `volumetricFarGridPixelSize` : uint | volume extents, group-reduce variant (`ComputeGridSizes`) | S, Sh |
| 6.1 | | `volumetricGridSizeZ`, `volumetricFarGridSizeZ` : uint | volume depth, `Slices()` | S |
| 6.1 | | `enabled`, `volumetricFogEnabled` : uint; `volumetricFogExtinctionScale` : float | existence of the volumes (1-texel when off) | T |
| 6.1 | | `fogDensity`, `fogDensity2` : float | ORG path on/off (derived `> 0`) | Sh (derived) |
| 6.1 | | `useSkyIBL`, `useDynamicCubemaps` : uint | client inputs | Sh |
| 6.4 | ScreenSpaceShadows (`BendSettings`, padded: check GPU use) | `SampleCount` : uint | program per scaled count | Sh |
| 6.4 | | `Enable` : uint | client | T |
| 6.5 | TerrainBlending (`Settings`, GPU-copied: split) | `Enabled` : uint32 | client existence; SSS depth source | T, Sh |
| 6.6 | ScreenSpaceGI | `ResolutionMode` : int | texture sizes, defines | S, Sh |
| 6.6 | | `EnableGI`, `EnableExperimentalSpecularGI`, `EnableTemporalDenoiser`, `EnableBlur` : bool | defines, texture set | Sh |
| 6.6 | | `Enabled` : bool | existence | T |
| 6.9 | PostProcessing pipeline | per effect `PostProcessFeature::enabled` : bool | effect list | Sh |
| 6.9 | CODBloom | `MipBlendFactor` : `std::array<float,8>` | derived used-mip count | Sh (derived) |
| 6.9 | DoF | `UseAdaptiveGather` : bool; `GatherQuality`, `BokehMode`, `HighlightShape` : int | key, pass selection | Sh |
| 6.9 | PhysicalGlare | `FFTResolution` : int; `ApertureMode` : int | FFT textures; key | S; Sh |
| 6.9 | LensFlare | `FFTResolution` : int; `GhostModeInt` : int | FFT textures; groups, blur passes | S; Sh |
| 6.9 | LUT | `LutPath` : std::string | LUT texture and type | S, Sh |
| 6.9 | ColorGrading | tonemapper selection (`currentTonemapper`, `useOpenDrt`, ...) | program | Sh |
| 6.10 | Effects11 (`SettingManager`, string-keyed) | `"EnableSunRays"` : bool | Sun Rays client and textures | T, S |
| 6.12 | VolumetricLighting | `ExteriorQuality`, `InteriorQuality` : int32; `ExteriorCustomSize`, `InteriorCustomSize` : `TextureSize{W,H,D}` | volume size | S |
| 6.12 | | `ExteriorEnabled`, `InteriorEnabled` : bool | engine VL on/off | T |
| 6.13 | LightLimitFix | `LocalShadowResolution`, `LocalShadowSlots` : uint | cache array size and slices | S, Sh |
| 6.13 | | `EnableLocalShadows` : bool | resources and hooks | T |
| 6.14 | Upscaling | `upscaleMethod`, `upscaleMethodNoDLSS` : uint | upscaled texture, client | Sh, S |
| 6.14 | | `qualityMode` : uint | `RenderSize` (constants only) | S (constants) |
| 6.14 | | `frameGeneration` : bool; `frameGenMethod`, `frameGenMultiplier` : uint | frame generation path | Sh |
| 6.15 | HDRDisplay (guarded by `settingsMutex`) | `enableHDR` : bool | `HdrEnabled`, formats | Sh |
| 6.16 | IBL | `EnableIBL`, `UseStaticIBL` : uint | client, EHF inputs | T, Sh |
| 6.18 | SubsurfaceScattering | `SSMode` : int; `EnableCharacterLighting` : uint | mode, passes | Sh, T |
| 6.18 | DynamicCubemaps (GPU-style struct: check) | `EnabledSSR` : uint | define | Sh |
| 6.18 | TerrainShadows | `EnableTerrainShadow` : bool | client | T |
| 6.18 | GrassCollision | `EnableGrassCollision` : bool | client | T |
| 6.20 | PhysicalLens | `ImageEffects`, `Diffraction` : bool | mapping cache client; diffraction passes in the build | Sh |
| 6.20 | | `Intensity` : float | ghost passes in the build (derived `> 0`) | Sh (derived) |
| 6.20 | Camera FX | effect `enabled` : bool | chain list (two passes, transient lens image) | Sh |
| 6.21 | ViewAtmosphere (`Settings`, GPU-copied by `FeatureBuffer.cpp:ViewAtmosphereBufferData`: split) | `nearPixelSize`, `nearSlices`, `middlePixelSize`, `middleSlices`, `distantPixelSize`, `distantSlices` : uint | atlas extent | S |
| 6.21 | | `enabled` : uint | medium, composite client, `PostWaterComposite` | T, Sh |
| 6.22 | PhysicalSky | `enabled` : bool | LUT, AP and cloud clients | T |
| 6.22 | | `enableVolumetricClouds` : bool | cloud passes, frame texture sources | Sh |
| 10c | DCLF (`ToggleSet`) | `cullMode`, `pipelineMode`, `interiorCpu`, `exteriorCpu`, `shadows`, `projectedUv`, `cpuRecord`, `cpuCadence`, `registered` | graph conditions, revision | Sh |

Not size-relevant (no port): MotionBlur `SampleCount` (constant only), SSS `BurleySamples`/`ScatterMode`, HistogramAutoExposure,
LocalExposure, Vignette, CloudShadows `Opacity`, Skylighting fields. Check TextureStreaming `BaseSize` (pushed through
`TextureStreaming::SetSettings`) and WetnessEffects `RaindropGridSize` when 6.18 is done (open question Q8).

---

## 7. Cross-cutting concerns

### 7.1 Memory peak during preparation

During preparation the old revision's textures are bound by frames in flight and the new ones are pending. Bound: **one revision in
preparation at a time** (step 4c), so the peak is the live set plus one closure's new textures, held until the retirement clock passes
the last frame that read the old ones. A display-size change doubles the display-size textures briefly. Measure on the RX 6600 with the
VRAM census during `resize` (step 4) and with a forced display-size change if Q4 allows one. Fatal if allocation fails (R8); no
fallback.

### 7.2 Recordings per change

A revision needs (member stages × 4 frame slots) recordings, fewer where parity variants halve them. Keep it down:
- the closure is derived, so only epochs that declare a changed slot or were rebuilt are members;
- unchanged member descriptions reuse kept recordings (`RequestDeclared`'s cache, keyed with the versions serial);
- one revision in preparation, newest inputs queued (a slider drag prepares the first and the last value, not every value);
- DCLF's scene revisions (every frame while moving) inherit the texture and build fragments, so a scene-only revision records only DCLF
  epochs, and a resize revision does not wait for a scene revision (assembler inheritance). **(Risk in step 10b; measure the census
  `frames from first wish to adoption` before and after.)**

### 7.3 Frame alias arena and re-made textures

See correction 3. Display-size textures (decision 6a) remove the most frequent re-make. Native engine targets are the only frame
transients that change size (window resize). A re-made group gets new reservations; the old ones are released when their last
reference goes. Watch `Transients::LogStats` blocks during a resize: the block count may grow by one and must return afterwards.

### 7.4 Constants from the adopted revision

All sizes in constants (EHF slices and grid, SSGI dispatch, LLF cluster grid, viewports, ratios) come from `Frame::Revision::Adopted()`.
The investigation's R5. Readers of another feature's sizes (EHF `Slices()` through shared data) read the adopted revision too.

### 7.5 History reset and the first frame

A new version of a `History` texture is zero-filled on the uploader before it is ready (step 4a). `Adopted().FirstFrame(texture)` is
true for exactly the first frame after the revision that gave it a new version. Features pass it to their temporal pass as "history
invalid". Frame-kind switches (load screens) do not reset history unless a feature declares that.

Previous-frame readers (`kPreviousFrame`) see the born-cleared contents on that first frame too, because they run before the
writer. Physical Sky's reflection faces are such readers: on the first frame after a new LUT version they read zero LUTs. That is
allowed by decision 5a, but a reader that would turn zeros into a visible artefact (black sky in the faces) should read
`FirstFrame` of the texture it reads and skip. A `kPreviousFrame` id whose writer and reader are in different stages makes both
stages closure members of a new version, which `EpochsDeclaring` already finds.

### 7.6 Shader reload

Step 7c. Today `RequestAllSynchronous` rebuilds every epoch on the owner thread with the host thread held. The end state is an event
whose revision has every epoch as a member. Open question Q5 on the pipeline swap.

### 7.7 Guest and native specifics

- **Guest.** CS stages run inside DXVK's stream; the flush latch stays as it is (late-bound). DCLF's build point runs at `Main::Draw`,
  after `Renderer::Begin`; step 10a moves DCLF adoption into `AdoptLatest` at `Renderer::Begin` (Q7). Own-epoch anchors (HDROutput,
  FrameGeneration hudless copy) become revision members in 7b.
- **Native.** The back buffer's versions and the swap chain colour space and format become revision inputs (9a). The HDR switch's
  black frames go away because the old chain keeps drawing until the new one is adopted. Frame generation is off in native (no change).
- **Ports with a per-mode source.** Physical Sky's AP shadow and cloud registers read the captured D3D11 view in guest and a zero
  view in native (no D3D11 pass writes them there). Guest's capture is late-bound. Native's zero view is a fixed declaration. When
  those passes move onto the graph, both modes read the graph twin and the per-mode difference goes. Native reports volumetric
  clouds off until then (`PhysicalSky::VolumetricCloudsEnabled`), so the clouds variable has a mode-dependent value, not a
  mode-dependent path.

### 7.8 The starvation rule (decision 4a)

Stated precisely:
- A revision in preparation may take any number of frames. Frames keep the last adopted revision.
- A newer event supersedes it (latest wins). Supersession is never an error.
- **Starved** = a recording the revision needs was dropped and asked for again (`EpochRecordingDropped`, a replaced staged build) more
  than `4 × kGameFramesInFlight` times while **no new event arrived for that revision's inputs**. Then `NoteTapError(TapError::Starved)`
  names the revision, the member epoch and slot still missing, and the drop reasons.
- A build or recording that fails is fatal at once (R8), not starvation.
- No timer and no frame-count limit. A long slider drag never fires it, because every drag step is a new event.
This replaces `FrameAnchors.cpp:Schedule`'s per-segment `kAskedAgainLimit`.

### 7.9 Diagnostics to keep

- The stale-recording tap error names the slot (`GraphHost::EpochRecordingStaleVersion`). Keep it.
- `[ORG] frame revision:` census (step 4): prepared, adopted, superseded, members and recordings per revision, wish-to-adoption frames.
- `[ORG] boundary:` census (step 1) until step 7; after that the fatal check replaces it.
- One info line per adopted revision with its changed variables and member stages (rate-limited like the frame-kind line).
- Tracy zones: `CS.Frame.Revision.Adopt` (render thread, must stay small), `CS.Frame.Revision.Prepare` (lane).

### 7.10 Shots and noise

Compare shots with `python tools/cs-test/cs_test.py compare <A> <B>`. Brightness varies run to run (eye adaptation). Recent docs
sections quote 0.1 to 1.8 for the quick configs and up to 26 for Dragonsreach's windows in the census; treat those as noise and look at
the image before calling a regression.

### 7.12 Readbacks, uploads, gates and lagging registrations (from the hard ports)

Each mechanism the ports added, what event drives it, what it carries and what replays.

| Mechanism | Port | Event | Carries | Replays? | Fits the model |
|---|---|---|---|---|---|
| LUT client at EarlyPrepass | Physical Sky | none (every enabled frame); `enabled` for the client | History LUTs, fixed size | yes: a gated pass in the pre-depth stage's recording | yes. The client is declared on `enabled` (6.22). |
| Disk phase LUT on change | Physical Sky | its 12 constants (CPU compare at commit) | a Persistent LUT, rewritten in place | yes: the gate is late-bound | yes. An on-change pass is a gate, never a revision. |
| Previous-frame reads | Physical Sky (faces) | none | last frame's LUT contents | yes | yes, with 7.5's first-frame note. |
| Per-draw latch handles | Physical Sky (sky, water families) | none | heap indices per draw | yes (late-bound) | yes. |
| Medium client with latched parameters | ViewAtmosphere | `enabled`, atlas sizes (6.21) | versioned Texture3D medium, parameters copied from the latch each run | yes | yes, once the atlas is declared on its settings. |
| Post-water composite with a lagging registration | ViewAtmosphere | `CompositeActive()` inputs | a client at EngineDraws, `PostWaterComposite` | yes | yes (6.21): `PostWaterComposite` is the registration the adopted revision holds, and the `Scheduled` gate is gone. |
| Skin material pool | Advanced Skin in DCLF | per-slot resolve each frame; `GpuTextures` replies and generation | table rows at t38, descriptor owners | n/a (FrameValues) | yes: late-bound. |
| Staged ghost paths | Physical Lens | a new lens model (CPU) | a `VersionedBuffer` version and the model fields it was built for | the post stage's recording binds the slot: the version must be adopted at a boundary | yes, as a `kBuffers` fragment (4e). Today: `AtNextBegin` (intermediate). |
| Lens caches (mapping, diffraction, ghost wave, planning) | Physical Lens | inputs compared at commit | Persistent textures and buffers | yes: gates, and the indirect argument seeds are copied from the latch every run, zeros when idle | yes. |
| GPU-argument draws and dispatches | Physical Lens | none | arguments written by compute | yes, by design (seeded every run) | yes. |
| DoF focus readback | Physical Lens | the camera active | a texel to the CPU a few frames late | **no** on frames that want it (an empty pass otherwise): a readback capture | conflicts with R9 (Q11). |
| Adaptation readback | Histogram Auto Exposure | every frame | the adaptation and histogram, the adaptation now feeding the pre-exposure | **no** on frames that want it (`ORGHistogramAutoExposure.cpp:ReadbackPass::Prepare`) | conflicts with R9 (Q11). |
| Lightning readback ring (planned) | Physical Sky | gated query | candidates to the CPU | **no** | conflicts with R9 (Q11). Design it after Q11. |
| Clean scene copy | Screenshot | a pending SDR capture | a Persistent texture | yes (own ticket today) | yes, after 6.23 and 7b. |
| Pre-exposure | the post chain, FSR, XeSS, DLSS | every frame (from the adaptation readback, `State::UpdateSharedData`) | one float in SharedData and in the SDK options | the value is late-bound | yes, but the SDK options are set on the render thread (`Streamline.cpp:EvaluateXeSS/FSR/DLSS` read `globals::state->preExposure`) while the evaluate records at execution later **(inferred race: in native the next frame's options can be set before this frame's evaluate records)**. The value must travel with the frame's evaluate, as the frame's other SDK options must (section 11, 4.3). |

The readbacks are the one real conflict with the plan. A revision records every member stage against pending versions and replays
the result, so a stage that cannot be replayed cannot be a member. Q11 decides how readbacks live beside replay.

---

## 8. Scaffolding-removed checklist (final form)

Run each grep from the worktree root over `src/` (and `extern/OpenRenderGraph`, `extern/ORGModuleServices` where named). Each must
return nothing (or only the stated exception) at the end.

| Item | Removed in | Proof |
|---|---|---|
| `RequestSize`, `RequestExtent`, `ApplySizeRequests` | 7a | `rg -n "RequestSize\|RequestExtent\|ApplySizeRequests" src` |
| `AtNextBegin` (RequestSize, Terrain Shadows, ImGui, Physical Lens's staged paths, Upscaling's test requests, and the hook itself) | 4b, 4e, 6.2, 6.3, 6.14, 7a | `rg -n "AtNextBegin\|atNextBegin" src` |
| Prepass size checks (`FollowSizes`, `CheckChainSteps`, `CheckImports`, `CheckWorldSteps`, `CheckStartSteps`, `CheckMenuSteps`, `CheckFlowMap`, `previousUpscaleMode`, LLF `pending` shape, `SettingsSizes` re-checks, `RegisterAgain`) | 6.x | `rg -n "FollowSizes\|Check(Chain\|World\|Start\|Menu)Steps\|CheckImports\|CheckFlowMap\|previousUpscaleMode\|RegisterAgain" src` and `rg -n "pending" src/Features/LightLimitFix/ORGLightCulling.cpp` |
| Resize `DrainCommandRing` calls | 6.14, 6.15 | `rg -n "DrainCommandRing" src/Features` (left: only fault paths in `FrameGenController.cpp`, if still needed) |
| `FrameAnchors::Service` resize poll | 7a | `rg -n "ResizeGeneration\|resizeGeneration\|ResizeCauses" src` |
| `ResizeGeneration`, `BumpResizeGeneration` | 7a | `rg -n "BumpResizeGeneration\|BumpGeneration" src` |
| `PublishedResources::Refresh` | 7a | `rg -n "PublishedResources::Refresh\|void Refresh\(\)" src/RenderGraph/PublishedResources.*` |
| `Rearm` for reset, one-shot `once`/`armed` passes | 6.6, 6.17, 7a | `rg -n "Rearm\(\|\.once\b\|armed" src/RenderGraph src/Features` |
| `ResetHistories`, `clearPending` | 6.1, 6.9 | `rg -n "ResetHistories\|clearPending" src` |
| `Frame::Resources` `fresh` | 7a | `rg -n "\bfresh\b" src/RenderGraph/Frame src/RenderGraph/Engine/ImageSpace*` |
| Per-stage staged switch (`SwitchStaged`, `stagedAsked*`, `kAskedAgainLimit` in `Schedule`) | 4e, 7b | `rg -n "SwitchStaged\|stagedAsked\|kAskedAgainLimit" src` |
| Version-lag branch in `Schedule` (`versionGeneration` bumps, `frameVersionsCurrent` exception) | 9b | `rg -n "versionGeneration\|frameVersionsCurrent" src/RenderGraph/FrameAnchors.cpp` |
| Mid-frame import publishing (`JoinSlot`/`EnsureViews` outside adoption) | 9b | `rg -n "JoinSlot\|EnsureViews" src/RenderGraph/FrameAnchors.cpp` shows calls only from the revision's adoption path |
| `EngineTargets` `Settle` builds and generation bump | 9a | `rg -n "Settle\(\|heldReplaced\|heldResized" src/RenderGraph/Engine/Targets` |
| DCLF frame-start wait | 10a | `rg -n "AdoptCurrentSnapshot\|WaitCurrentSnapshot\|kCatalogTimeout" src` |
| `RenderGraphRuntime::AttachDclfBuilds`, DCLF `external` epochs, frame-end `Attach::All` for DCLF | 10a | `rg -n "AttachDclfBuilds\|Attach::All\|\.external" src` |
| DCLF `snapshotSlot`, separate `AdoptSnapshot` adoption, `StaleKindOf` generation checks | 10b | `rg -n "snapshotSlot\|StaleKindOf\|kStaleBuild\|kStaleTargets\|kStaleMain\|kStaleShadowFormat" src` |
| `Toggles::BeginFrame`/`Adopt`, `RefreshGraphConditions` polling | 10c | `rg -n "Toggles::BeginFrame\|Toggles::Adopt\|RefreshGraphConditions" src` |
| `Builds::RequestAllSynchronous` for shader reload | 7c (or kept with a recorded reason, Q5) | `rg -n "RequestAllSynchronous" src` |
| Boundary check's allowed callers (`dclf`, engine-import exception) and its counted (non-fatal) mode | 10a, 9b, 7d/10c | `rg -n "Allowed\|allowedCaller\|kCounted" src/RenderGraph/Frame/BoundaryCheck.cpp` |
| Writer shims: `Feature::SettingsWritten`, `ProjectSizeSettings`, `SizeProjection`, and the wrappers' event calls | 7a | `rg -n "SettingsWritten\|ProjectSizeSettings\|SizeProjection" src` |
| The lane's `RequestSize` input and the coordinator's "wish" path | 7a | `rg -n "Wish\|wishes" src/RenderGraph/Frame/Revision.*` |
| Structural rebuilds from publishing under an existing id (EHF, SSGI, LLF, adapt pair, flow map) | 6.x | `rg -n "RequestStructuralRebuild" src` shows only revision-driven shape-class changes; the census shows no `structural rebuild` after `resize` and the toggles |
| `GraphResources.h` header rules for `RequestSize`/resize generation | 7a | `rg -n "RequestSize\|resize generation" src/RenderGraph/GraphResources.h` |
| The guest own-epoch path that takes builds mid-frame | 7b | `rg -n "own epoch" src/RenderGraph/Frame/FrameDriver.cpp` (comment gone) and no `OwnEpoch` build attaches in the census |
| Schedule gates for clients registered inside the frame (ViewAtmosphere's composite, the screenshot's clean scene) | 6.21, 6.23, 7a | `rg -n "FrameAnchors::Scheduled\|bool Scheduled" src` |
| The lens's per-frame upload poll (`StagedBuffer::Poll`) | 4e | `rg -n "StagedBuffer\|Poll\(\)" src/Features/PostProcessing/ORG/ORGPhysicalLens.cpp` |
| Readback captures inside CS stage recordings | Q11's answer | `rg -n "PlanReadbackCapture" src` shows only the places Q11 allows |
| The screenshot's per-capture size and format compare | 6.23 | `rg -n "const bool same" src/Features/Screenshot/ORGScreenshot.cpp` |

Kept on purpose (not scaffolding): `ComposedVersions`, `NamedVersions` texture entries, `AttachStagedEpochGraphs`, `EpochsDeclaring`,
the ORG adoption observer (development check), the missed-writer check, `Setting<T>` and its registry, late-bound data paths.

---

## 9. Open questions and unknowns

| # | Question | How to resolve |
|---|---|---|
| Q1 | Exactly which data `EpochsDeclaring` should use: the graph's declared versioned resolvers at build, or each recording's `versions` list. | Read `ORG/src/Render/RenderGraph/RenderGraphPersistent.cpp` around the comment "the one its host data's revision names" and where `PersistentRecording::versions` is filled. Prefer build-time data (staged graphs have no recording yet). |
| Q2 | Cost of making a twin's D3D11 export at adoption on the render thread (DXVK interop wrap of existing memory). | Tracy zone around `AdoptPrepared` in a `resize` run. If it costs more than about 0.2 ms per twin, investigate making exports on the lane (D3D11 device creation is free-threaded, the interop call is not known to be). |
| Q3 | Which executor the revision lane runs on. | Read `RenderGraphRuntime.cpp` for the executors used by `BuildManager`'s build thread and DCLF's pumps (`DCLF/Core/Scene/Coordinator/ScenePump.cpp`). Do not add a thread if a serialized pump on an existing executor works. |
| Q4 | Does the engine use its new targets between `BSShaderRenderTargets::Create` and the next `Renderer::Begin`? Can a window resize happen in a session at all? | Grep run logs for "creating its render targets again". Read `Hooks.cpp`'s thunk caller (`Renderer::WindowSizeChanged`). If it never runs mid-session, implement 9a's event path without a test and say so in the dated doc. |
| Q5 | Can shader reload's pipeline swap happen inside `AdoptLatest` (host thread held briefly) instead of `RequestAllSynchronous`? | Read `RenderGraphRuntime::ServicePipelineReload` and ORG's `BuildIfRequested` with the host held. |
| Q6 | Where the engine shadow map's size and format are created and whether they change in a session. | Read `LightLimitFix.cpp:EnsureLocalShadowResources` (it reads `kSHADOWMAPS`) and the engine target plan in `docs/development/p4/render-targets.md`. |
| Q7 | In guest, do any CS stages submit between `Renderer::Begin` and DCLF's `Main::Draw` build point? If yes, moving DCLF adoption to Begin changes nothing for them; if DCLF's guest build point must stay at `Main::Draw`, adoption for DCLF members must still be at Begin. | Read `FrameDriver.cpp:OnBegin`'s comment and `DCLF/Core/Render/FrameDriver.cpp:BeginFrame`'s caller in guest; check the epoch order in a `guest-quick` census. |
| Q8 | TextureStreaming `BaseSize` and WetnessEffects `RaindropGridSize`: size-relevant or not. | Read `TextureStreaming.cpp:SetSettings` and the wetness shaders. Port only if a texture size follows them. |
| Q9 | How often `AttachStagedEpochGraph` is refused in practice. | Step 3's counter over the full test set. Any refusal after step 4 is fatal by design; if refusals occur in normal runs, find the configuration change that causes them. |
| Q10 | A7 (HDR output as a UI-stage step) is in another plan. If it lands first, 7b shrinks. | A7 landed 2026-11-19 (`org-native/fg-a7`): the hudless copy, the clean scene copy and HDR output are UI steps, and no anchor runs an epoch of its own. 7b is left with `SwitchStaged` and FrameDriver's mid-frame build comment. |
| Q11 | **Decided 2026-11-19: (b), replayable readbacks.** The post chain already uses this form in CS (`RunReadback` in `ORGPostProcessing.cpp`: a host buffer with a region per run lease, a stamp and the data copied by revisioned copy passes, read when the lease comes round). Readbacks against replayed recordings (R9). A revision records every member stage once per frame slot and replays it, and a readback capture is never replayable. Today the post stage holds the lens focus and adaptation readbacks, and the planned lightning ring would put one in the pre-main stage. The post stage already fails with the R9 error when the Post Processing chain runs on the graph (being fixed separately). | Options: (a) readbacks leave CS stages: each runs on a placed epoch or ticket of its own after the stage, reading the published texture; (b) ORG gets a replayable readback: the copy targets a ring entry picked by a run-latched index, and the CPU learns completion from the retirement clock instead of a lifecycle callback; (c) a stage that holds a readback is recorded every frame and never replayed, which makes it unable to be a revision member, so the readback would block any revision touching that stage. Recommended: (b), since it keeps one stage and no extra epochs, and Physical Sky's lightning ring is already designed that way (ring entry per frame in flight, non-blocking completion query). Whatever fixes the post stage now should be checked against this choice. |
| Q12 | **Decided 2026-11-19: the recommendation.** Keep the gate until 6.21 and tie `PostWaterComposite` to `Scheduled`. Lagging registrations (`FrameAnchors::Scheduled`) as an interim. ViewAtmosphere's composite and the screenshot's clean scene skip frames until their build holds them. Before 6.21 and 6.23 land, keep the gate, or make the switch of `PostWaterComposite` wait for the same frame as the composite? | Recommended: keep the gate until 6.21, but tie `PostWaterComposite` to `Scheduled` (not to the registration) so the frames between have the shaders' own fog instead of none. That is a point fix, deleted by 6.21. |

**Decided by the agent (step 1, 2026-11-19), to confirm:**
- **Render thread.** The census counts the thread that last opened `Boundary::Scope` (the thread that runs `OnBegin`), not
  `FrameAnchors::IsRenderOwner`. It keeps `BoundaryCheck` free of `FrameAnchors.h`. A loading screen's thread that begins frames
  takes the count over while it does.
- **Late changes cover every build request.** Besides `Register`/`Unregister`/`Refresh`/`Rearm` and `RequestStructuralRebuild`, every
  `RenderGraph::Builds::Request/RequestAll/RequestFor/Change` call is counted. Many requests reach neither (an import added, engine
  targets resized, DCLF's graph conditions, a published resource added, a stage key change). A `Boundary::Change` scope at the named
  sites keeps their own requests from counting twice.
- **Late attaches are counted at the attach.** In `BuildManager` (`Finish` for an attached build, `SwitchStaged`), keyed by epoch and
  build point (the `Poll`/`BuildOwnerEpochsNow` caller). `Schedule`'s switches run inside `OnBegin`, so they never count. The
  frame-end `Poll(Attach::All)` and guest's DCLF build point at `Main::Draw` count as late: both are section 8 scaffolding.
- **More in the census line.** It also gives adoptions inside the scope ("at Begin"), the allowed `dclf` adoptions apart, the callers
  of late versions, and session totals, so the last line of a run is its total.
- **A new resource's first version is counted apart** ("new resources made late"). It replaces nothing a recording bound. In the first
  runs nearly every late version was a first version (one per resource), which hid the replacements. Mid-frame imports still show
  there, for step 9.
- **The census also logs at a test run's exit** (`Testing::RegisterExitReport`). The `[ORG] declared frame:` window
  (`Driver::LogStats`) never closes in a quick run unless `CS_ORG_EPOCH_STATS=1`, so without this a quick run printed no census.
- **Caller tags.** `dclf` (allowed, `AdoptSnapshot`), `engine import` (`FrameAnchors.cpp:JoinSlot`), `twin`
  (`GraphResources.cpp:Resource::JoinSlot`). Other sites are `untagged` and named by resource.
- **ORG test in its own executable.** `tests/VersionedResourceTests.cpp` (`OpenRenderGraphVersionedResourceTests`, WARP), not a
  scenario in `RebindingGpuTests.cpp`: `OpenRenderGraphExternalResourceTests` stops early on this base with an uncaught
  `std::system_error` ("resource deadlock would occur"), also without this change, so a scenario there never runs. A standalone ORG
  configure also needs `-DDirectX-Headers_DIR` (ORG's `vcpkg.json` lacks `directx-headers`, which BasicRHI requires).
- **Since the plan was written.** A7 removed the own-epoch anchors, so guest's late attaches come from DCLF's build point and the frame
  end only. Native swap chain replacement runs on the submission thread, so its adoptions are not counted. The back buffer's
  per-present version (`NativeDevice.cpp:NextGraphVersion`) is on the render thread and counts. Physical Sky's handle tables and
  `RunReadback` adopt no versions.

**Decided by the agent (step 2, 2026-11-19), to confirm:**
- **Misses count only the set's own kind.** A buffer set counts `Find` misses, a texture set `FindTexture` misses. Asking for
  the other kind is not a miss. DCLF's named parity check (`NamedParity.cpp:CheckNamedResolves`) warns on any growth of a buffer
  set's `Misses()`, and DCLF's recordings resolve CS textures through the same set, so counting those would raise false warnings.
- **Texture sets hold textures weakly**, like buffer sets. `AdoptAll` skips an expired one. `Textures` throws on a null texture
  or version and on a texture named twice. It takes `NamedVersions::TextureEntry` pairs (texture, version).
- **`ComposedVersions` is a constructor** over up to four `IResourceVersions` (null parts skipped, a fifth throws). It gets its
  own serial from the sequence `NamedVersions` uses (`src/VersionsSerial.h`), so one cache key space covers both. The lane makes
  one per revision, so recordings of a revision share it.
- **The CS argument carries a serial.** `IResourceVersions` has no serial and ORG stays untouched, so `RequestDeclared` takes
  `RecordVersions { versions, serial }`. Versions without a serial, or a serial without versions, is fatal. Serial 0 is "current
  versions", today's only caller.
- **The ORG test moved to OMS, at resolver level.** Step 3 edits ORG at the same time, so ORG is untouched. `ORGNamedVersionsTests`
  (WARP) resolves through `VersionedResource::SelectVersion` with host data naming a `Make`d texture version, which is the call a
  recording's preparation makes: the pending version is selected, then the same one after `AdoptAll`.
  `EpochRecordingVersionsCurrent` compares version numbers per slot whatever the resource kind, and the buffer case is covered by
  `OpenRenderGraphPersistentVulkanHostTests`. A texture case there (record against a `Make`d texture, not current, current
  after `Adopt`) is left for after step 3 merges. OMS tests need a parent project with ORG's targets: a two-line CMake project
  that adds `extern/OpenRenderGraph` and `extern/ORGModuleServices` with CS's ORG cache settings (BasicTelemetry artifacts off,
  UIs off), `-DORG_MODULE_SERVICES_BUILD_TESTS=ON -DORG_MODULE_SERVICES_ENABLE_DXC=OFF`, and the worktree's
  `build/Dev-Fast/vcpkg_installed` (`x64-windows-static-md-release`). ORG needs Vulkan and D3D12 both on.
- **Since the plan was written.** Frame generation (steps 1-8) and Physical Sky's port added no other segment host data. The only
  other host data naming versions is DCLF's `EpochRevisionData`, which keeps using its buffer `NamedVersions` directly.

**Decided by the agent (step 3, 2026-11-19), to confirm:**
- **Q1: build-time data, kept current.** Each graph keeps the set of versioned resources its passes declare
  (`RenderGraph::PersistentDeclaredVersions`, keys of `PersistentInstanceState::entries` with a `versioned` slot). Its first build
  sets it (`PrepareFirstPersistentBuild`, so a staged graph answers before it is ever prepared) and each preparation refreshes it,
  only when a versioned slot was added or a slot retired. It is not the recordings' `versions` lists: a staged graph has none.
  A graph built on the owner thread (`BuildIfRequested`) answers nothing until its first preparation; CS's staged builds all go
  through `BuildEpochGraph`, so that never matters for a revision.
- **`EpochsDeclaring` takes `IVersionedResource`**, not `VersionedResource`, and matches by `Key()`, so a clone matches its original.
  The host keeps each instance's attached and staged sets under a mutex, published on the owner thread at install, staging and
  retirement, so the query is safe on the revision lane.
- **"Not being submitted" means:** no ticket of the epoch is held (`HoldEpochTicket`), and the call is not inside a submission on the
  same thread (without a submission thread). The single form never checked either. CS holds no tickets today.
- **"No newer build" means a newer build attached** (the single form's serial check). A build still running is not visible to the
  host; superseding it is the revision lane's job (latest wins).
- **Check order.** The set itself first (unknown epoch, an epoch named twice), then each epoch in the order given. The result names
  the first refused epoch.
- **Refusals counted twice over.** ORG counts every refusal of `AttachEpochGraph`, `StageEpochGraph` and both staged attaches by
  reason (`AttachStatsTotal`, host lifetime). CS counts the ones `BuildManager` meets (staging, attach, switch) in the
  `[ORG] boundary:` line as "build refusals", by site and reason, on any thread. That is Q9's counter.
- **CS does not call the new functions yet.** Step 4 does. The plan's acceptance is "CS unchanged in behaviour".
- **Since the plan was written.** Native frame generation (A7, swap chain replacement on the submission thread) and Physical Sky's
  full port change nothing here: staged builds still switch per epoch through `FrameAnchors.cpp:Schedule` and
  `BuildManager::SwitchStaged`, which step 4 replaces with one `AttachStagedEpochGraphs` per revision.
- **`OpenRenderGraphExternalResourceTests`** still stops early (exit 127 on this base, after the device is made, before any scenario
  line); not fixed here. The step 3 test is a scenario of `OpenRenderGraphPersistentVulkanHostTests built` instead, which runs on the
  RX 6600.
- **`PersistentVulkanHostTests leases`** fails with "Synchronous frame capacity exhausted" and then hangs, on ORG `2b22711` as well;
  not fixed here.
- **Q9 answered for the quick set:** 0 refusals in `guest-quick`, `native-quick`, `native-d3d12-quick`, `d3d11+quick`.

**Decided by the agent (step 4, 2026-11-19), to confirm:**
- **4c and 4d were one worker**, since the coordinator cannot be tested without adoption. 4a ran alongside 4b as planned.
- **Q2 answered: exports at adoption are cheap in guest, not in native.** `AdoptPrepared` wraps on the render thread (option (a): the
  image and graph resource are made on the lane in the previous version's home layout, the wrap's layout is checked, fatal if it
  differs). Guest 0.007 to 0.053 ms per twin. Native Vulkan 19 to 75 ms, native D3D12 41 to 102 ms per exported twin (Dev-Fast, /Od):
  `NativeDevice.cpp:WrapTexture` runs `Initialize` (an immediate submission that waits) and a `RunOp` clear on the render thread.
  Today's `Resize` paid the same. Wrapping on the lane was rejected: DXVK's wrap injects a chunk into the immediate context
  (`LockImage`/`PinGeneral`), and native's waits. **Open:** in native, give exported versions `Contents::Zero` on the lane and skip
  the wrap's initialize, so adoption does not wait.
- **Q3 answered: the lane is a `SerializedTaskPump` on DCLF's `SceneExecutor`** (`DCLF::SceneScheduler::MakeLanePump`, scope "CS frame
  revision"). `BuildManager` runs a bare thread with its own queue, not an executor. DCLF joins revisions in step 10 anyway.
- **Zero-filled versions are `QueueSharing::Concurrent`.** The copy-queue zero fill needs no ownership transfer then. Only graph-only
  History twins are Zero. Exported versions are `Undefined`: their wrap at adoption zero-fills them, and nothing may touch the image
  before that (it is still UNDEFINED). Depth, MSAA and multi-device textures are refused for Zero.
- **Twins are snapshotted on the render thread** (`PlanPrepare`), and the lane makes the version from the snapshot. The lane never
  touches a twin, so a twin destroyed meanwhile needs no lock.
- **Wishes reach the lane only at Begin**, so the twins one feature resizes in a frame land in one revision (about a frame of latency).
- **The render thread owns the `RevisionAssembler`.** The lane makes versions and the closure. Recordings are requested at Begin
  (descriptions and schedules are render-owner state); the Begin that finds everything ready seals and selects. So "superseded"
  counts only revisions dropped because a twin was destroyed or remade (0 in every run).
- **A newer wish does not supersede the revision in preparation.** It is prepared after that one is adopted (a drag adopts the first
  and later values, 7.2). A twin wished to A and back may briefly adopt A.
- **Only slots whose schedule is current get a revision recording.** `Schedule` fills empty slots against the adopted versions
  afterwards. `NoteNewVersions` is kept after `AdoptAll` for per-frame tickets (DCLF).
- **Staged builds join the revision in preparation**, or a builds-only revision when none is (while a build is staged the attached
  graph takes no new recording). A staged member records all four slots on the staged build's description. A new staged generation
  is a new input for the starvation rule, not a drop. The rule lives in the coordinator only.
- **`Builds::SwitchStaged` was removed in 4e, not 7b**: since A7 every staged epoch is a CS declared segment, so it had no caller. It
  became `Builds::AttachStaged(epochs)` over `AttachStagedEpochGraphs`; a refusal is fatal.
- **The version-lag branch stays** for slots published outside revisions; each use is counted by slot in the `[ORG] boundary:` line
  ("version lags outside revisions"). In the runs: `TerrainShadows::ShadowHeight` (resized directly at Begin, 6.x) and
  `ImGui::Texture N` (`AtNextBegin`, 6.3).
- **The buffer fragment is CS-local** (`RevisionBuffers` in `Revision.cpp`, an `IResourceVersions` over pending buffer versions and
  their payloads), not OMS: no second user yet. Buffer versions are made and uploaded on the lane.
- **Not done: non-preparable wishes** (another shape class) still take `ResizeTo` at Begin and a structural rebuild
  (`Revision.cpp:ApplyResizes`). No run hit it. The end state is a revision with a new slot and a `kBuilds` fragment.
- **Since the plan was written.** Native frame generation and Physical Sky's port changed nothing here. Guest's 2 to 4 late versions
  are `Engine::3D::VOLUMETRIC_LIGHTING_ACCUMULATION(_COPY)` (twin `JoinSlot` of engine targets), present before step 4; no late
  version comes from a `RequestSize` site. "Declared view mismatch" lines remain (18 per resize run); not checked one by one that
  each is a DCLF reader.
- **Native self-test.** `CS_ORG_GRAPHRES_SELFTEST=1` fails its first (old) part in native, `16x16 MISMATCH: D3D11 read 0`, on the base
  stage too, so the prepared part is tested in guest only.

**Decided by the agent (step 5, 2026-11-19), to confirm:**
- **Q6 answered.** `kSHADOWMAPS` is made once, in `BSShaderRenderTargets::Create` (through `CreateDepthStencilTarget`, which
  `EngineTargets` hooks). A second `Create` is fatal (Q4), so `EngineShadowMap` has one event per session, at `EngineTargets.cpp:EndSet`.
  Its size following `iShadowMapResolution` is inferred.
- **Variables are typed setters and getters, not handles.** `Frame/Variables.h` exposes `SetDisplaySize(value, writer)` and
  `AdoptedDisplaySize()` and so on, so features never see OMS types. Writers pass a `std::string_view` writer name.
- **Declaring a variable is a change** (OMS): it wakes and bumps the set's serial, so late-declared per-feature variables reach the
  lane. `Derive` rejects a `sizes = true` result over a `sizes = false` input. A throwing derive function is fatal through `Take`.
- **Setters record in every runtime mode** when the graph is compiled in, so `CS_ORG=0` logs `[ORG] variable` lines too. Only the
  compile-time stub is a no-op. Variables-only revisions also run without a graph host.
- **A variables-only revision is adopted in the Begin that starts it.** A change while a revision is in preparation waits for the
  next one. A value set A to B and back to A before a revision starts still lists the variable as changed (its serial moved).
- **`HdrOutputTarget` is written each frame from `BeginRenderFrame`** (address, size and format of `PostChainTarget()`).
  `UpscalerOptions.hdr` is the HDR display state; the SDK's `colorBuffersHDR` is always true today.
- **`CS::Setting<T>` needs a trivially copyable `T`.** `RaiseAt` and the check compare raw bytes with the last raised bytes. Every 6.4
  field fits. `SettingRegistry::Declare` returns the existing id for a name declared again. A bound setting's destructor detaches it.
  The raise hook takes `(id, name)` and is wired to `Vars::RaiseSetting` in `Revision::Install`. Widget overloads cover `bool`,
  `int`, `uint32_t` and `float` only.
- **The setting pilot is the graph resource self-test** (`CS_ORG_GRAPHRES_SELFTEST=1`, `setting pilot OK`), since it has a switch and
  log lines cs-test reads. The generator path is covered by a Python test.
- **Nested keys** match a member with its spaces dropped (`CODBloom` finds `"COD Bloom"`) and look inside an object's `"settings"`
  member, where PostProcessing keeps each effect's settings. The flat form is tried first.
- **The projection is a list of named fields** (`Feature::SizeProjection`, integer, real or text), kept in a side table in
  `Feature.cpp` so the `Feature` layout is unchanged. The first projection of a feature is its baseline and raises nothing.
  Derived fields project the derived value (EHF `fogDensity > 0`, CODBloom used mips, PhysicalLens `Intensity > 0`,
  `volumetricFogExtinctionScale > 0`).
- **The fog grid fields are projected under ViewAtmosphere**, where they live now (`nearSlices` and the rest). EHF keeps its toggles.
- **`SettingsWriteBatch`** holds a feature's events while a scene layer is suspended (`SceneLayerGuard`, `RestoreFeatureLayers`), so a
  round trip raises nothing. The menu's per-frame `SettingsWritten("menu")` after `DrawSettings` covers the scene widget
  interceptor's copies.
- **The missed-writer check reports a field only when two checks in a row see it**, so a writer on another thread caught between
  its write and its `SettingsWritten` is no miss. Its tap error (`TapError::MissedSettingsWriter`) is fatal under
  `CS_ORG_MISMATCH_FATAL`, as tap errors are. The check found writers the 6.2 table lacks, now routed: cs-test's `hdr` command and
  the native menu's HDR and SSGI rows (`NativeMenu/Rows_HDR.cpp`, `Rows_SSGI.cpp`). The FG fault teardown
  (`Upscaling::PresentWithFrameGeneration`) raises too.
- **Writers left on purpose.** `LoadSettings`/`RestoreDefaultSettings` calls inside a feature's own `LoadSettings` (the outer
  wrapper covers them), `PostProcessing::SyncPipelineResources` (covered by the call after `UpdatePipeline`), CSEditor widgets.
- **Upscaler options are not read from the adopted revision yet.** `ConfigureUpscaling` computes the render size from the live
  `qualityMode` in the evaluate's frame. Reading only the options from the adopted revision would pair an old mode with a new
  render size. It moves with `RenderSize`/`DynamicRatios` in 6.14.

**Decided by the agent (step 6.1, 2026-11-19), to confirm:**
- **Declarations are evaluated at the start of a revision, on the render thread**, not on the lane. Description and key functions
  are cheap, and the client registration needs the render owner. Versions are still made on the lane. Evaluating only when a
  revision starts (never while one is in preparation) keeps a staged build of a new client from joining an older revision.
- **Texture descriptions may change only their extent.** Another format or flag is fatal. No 6.x user needs more yet; a shape-class
  change needs a new slot and staged builds of its readers, left for the first user that needs it.
- **No `DeclareProgram` yet.** EHF's programs are fixed at Setup. Programs that follow settings (6.4, 6.6) can be part of a client's
  key (its `make` loads them) or get their own declaration then; loading on the render thread at revision start would block a Begin
  for a compile, so 6.4 should decide.
- **Client keys may read state that is not a variable** through `DeclaredClient::Invalidate()` (an interim). EHF's inputs (IBL,
  Skylighting, LLF, Terrain and Cloud Shadows, Physical Sky publishing, the directional shadow map, temporal) are found by its Prepass;
  a change invalidates the key and the next revision registers the new client. They become variables as those features migrate.
- **The first enable registers the client one revision after the volumes.** The key needs one Prepass's inputs, so the volumes and
  values adopt first and the client about four frames later. Volumetric fog stays off in those frames (below). A later enable reuses the
  inputs already seen, so all of it switches in one revision.
- **The shaders' volumetric flag follows the written volumes.** On the graph, `GetCommonBufferData` clears `volumetricFogEnabled`
  while `NearSRV()` is null (no run wrote the bound versions yet). Every reader then skips the volumes in the frames after a new
  version, instead of the D3D11 readers getting null views and the graph readers an unwritten volume. A frame the graph path cannot
  run (inputs missing) has no volumetric fog; it no longer falls back to the D3D11 path (debug mode 3 still runs it).
- **Declarations live until process exit.** Dropping a `DeclaredClient` stops following and leaves the registration; unregistering
  from a static destructor at exit was the alternative and is unsafe.
- **Debug mode 10 (one-shot history clears through `Rearm`) is removed**, so EHF has no `Rearm` or `once` pass left. Mode 9 stays.
- **ViewAtmosphere's four grid fields were ported in 6.1**, since EHF's volumes depend on them. 6.21 ports the rest.
- **Density and extinction drags raise events.** They are plain `Setting<float>`; the declarations quantise them (`VolumesOn`), so a
  drag adopts variables-only revisions and never a new version.

**Decided by the agent (step 6.2, 2026-11-19), to confirm:**
- **A general texture input, not an ImGui variable.** Pages change on ImGui's texture requests, not on a value, so they are not
  variables or declarations. `PublishTexture` sits beside `PublishBuffer` in `Revision.h` and is small and additive. Terrain
  Shadows (6.3) could publish its height map the same way if it keeps its own twin.
- **The page's contents stay written in the frame.** The version is not uploaded on the lane: ImGui's pixels change on the render
  thread between requests, and the existing upload pass already restages a texture whole until a recorded copy wrote its bound
  version. So the version is born undefined and written by the first frame that binds it.
- **A new atlas texture draws a few frames later than before.** It is drawn once a revision adopts it (its UI stage recordings
  are made first), not at the next Begin. Text on a re-created atlas is missing for those frames, as it was for one frame before.
  The `menu` config's font reload shows it.
- **No fallback without ORGModuleServices.** A build with the graph but without OMS has no revisions, so pages would keep their
  placeholders. Every real build has OMS (DCLF needs it).

**Decided by the agent (step 6.3, 2026-11-19), to confirm:**
- **`HeightMap` is a frame variable** (`Vars::HeightMap()`, `HeightMapIdentity { id, size }` in `Frame/Variables.h`, an additive
  change). The id counts `ORGTerrainShadows::SetHeightmap` calls in the session, so a worldspace visited again is a new value.
- **Only the shadow height map is a declared texture.** The heightmap itself (its contents, R16 or BC4 as the file has them) stays
  a plain Persistent texture made per heightmap and held by that heightmap's client, which uploads it once. A declared texture may
  change only its extent, and a new heightmap is a new client anyway.
- **The client's make takes the heightmap its key names** from a map of staged heightmaps, and drops the older ones. A revision
  that started with an older value registers that one, and the next revision registers the newest.
- **Failures after Setup are fatal.** The mid-session fall-back to D3D11 (`Stop`, the `onFailed` callback, the reload block in
  `TerrainShadows::EarlyPrepass`) is gone: it unregistered the client and withdrew the published id outside a revision.
- **The shaders' scale and offset still read the live heightmap size.** `NeedsRefresh` keeps terrain shadows off until a refresh
  ran in a graph holding the new client, which is adopted with the new version, so the two sizes agree whenever shadows are on.
- **The heightmap upload stays a one-shot pass** (`PassDesc::once`). The one-shot machinery is step 7a's.

**Decided by the agent (step 6.8, 2026-11-19), to confirm:**
- **The grid is a run value, not a recorded constant.** It goes into the latch, and the dispatches cover the Setup grid. A push
  constant read from `Adopted()` would make every ratio change a new pass revision, so a re-recording outside any revision.
- **The grid applies the adopted ratios always.** `Util::ConvertToDynamic` ignored the ratios while the engine's
  `dynamicResolutionLock` was set; that is live engine state, not a variable. With an upscaler the grid may now be smaller than
  before (64 render pixels a cluster). Watch for lights dropped by `CLUSTER_MAX_LIGHTS` in dense scenes.
- **No buffer declaration.** Declare.h has none, and the buffers already follow the display size at Setup. A display larger than at
  Setup (a native swap chain resize) keeps the buffers and gets larger clusters, with one warning. Declaring the buffers on
  `DisplaySize` waits for a buffer declaration.
- **Before the first adopted `DisplaySize`, the grid is the Setup grid.**
- **Guest also copies the whole light capacity each run** (1024 lights, about 80 KB), as native did. Its recordings then stop
  changing as the light count grows.

**Decided by the agent (step 6.12, Volumetric Lighting), to confirm:**
- **`Interior` is a frame variable** (`Vars::Interior()`, `SetInterior`), not a feature setting, since other features may want it.
  Its only writer is Volumetric Lighting's EarlyPrepass, every frame, so it is not written while that feature is off.
- **The engine is given the adopted values.** EarlyPrepass gives the engine its VL on/off, quality and High-slot size from
  `Adopted()` (live settings without the graph), so the engine's descriptor size is the bound version's. A mismatch at the VL
  render is fatal. This assumes `SetVLQuality` changes the descriptor size at once; the old code's re-registration hints that
  it does, but it is not checked. Until a revision is adopted the engine renders with the old values (a few frames of the
  exterior's quality after entering an interior). Menu edits no longer call `SetupVL` at once, in D3D11 mode too (next frame).
- **One persistent volume** at the adopted location's size replaces the two transient ones (exterior and interior sizes). A
  non-exported transient cannot be versioned (`Resource::Aliasable`), so a transient's new size would be a structural rebuild.
  It is one texel while the adopted location's VL is off (existence rule). The custom size's memory (up to 640^3 R16F) stays
  allocated while that size is in use, as before.
- **The feature off (native's engine VL)** declares the volume once at the engine's size, with no dependencies, at Setup or, when
  the descriptor has no size yet, at the first VL render (a first version made mid-frame). A later size change is fatal.
- **The client has no dependencies.** Its key is the sky's noise gradient view and Terrain and Cloud Shadows' published ids, which
  the VL render finds and invalidates on a change (the 6.1 interim). The engine-target identity check is gone (Q4: a mid-session
  `Create` is fatal elsewhere). Each registration keeps its own pass list and commit, so the old build draws until adoption.
- **`ExteriorEnabled` and `InteriorEnabled` are ported too** (`Setting<bool>`), so `ProjectSizeSettings` is gone for the feature.
- **A frame without VL after the first one ran logs a warning** (the first 16, "has no volumetric lighting after the first one
  ran"), so a test can show none.

**Decided by the agent (step 6.5, Terrain Blending, 2026-11-19), to confirm:**
- **The frame follows the adopted `Enabled` on the graph path.** `TerrainBlending::FrameEnabled()` is the adopted value while
  `ORGTerrainBlending` is active, and the live setting otherwise (plain D3D11 untouched). The depth hook, the draw hook, the deferred
  terrain passes, `TerrainShaderHacks`, the HiZ source, `Util::GetCurrentSceneDepthSRV`, the GPU `Enabled` and DCLF's
  `DefersTerrain` all read it, so they switch in the revision that switches the blend's client. It is read once a frame (keyed on
  `State::frameCount`), since the draw hook asks per pass. DCLF's classification runs off the render thread and reads the last
  render-thread answer (`LastFrameEnabled`).
- **The blend's key is its depth imports**, observed at every depth pass while the depth table holds the engine's views (`Observe`,
  on and off), with `Invalidate` on a change. The first enable after startup can still be one revision late if no depth pass has run.
  The frames between blend on D3D11, as a missed anchor did before.
- **Group counts come from the bound version at commit** (`Adopted().Extent`), so a new display size is a new version and no new
  registration.
- **The terrain depth twin stays made once.** It is not declared on `DisplaySize`: its D3D11 views live in `terrainDepth`, read by the
  engine's draws and DCLF's import of the plain resource, which would have to follow a slot first. Guest never changes the display
  size; native's window resize already left it at its first size. Its clear is now a declared client on `Enabled`.
- **The GPU block is split** (`GpuSettings`, the same 16 bytes). `ProjectSizeSettings` is gone.

**Decided by the agent (step 6.4, Screen Space Shadows and `DeclareProgram`, 2026-11-19), to confirm:**
- **`DeclareProgram(name, DependsOn(...), key, build)`** in `Frame/Declare.h`, for 6.6 (SSGI's `recompileFlag`), 6.15 (HDR's
  `programDirty`), 6.18 (SSS's and Dynamic Cubemaps' `programsDirty`) and Effects11. `key` (render thread) gives what the program is
  built from, `build` (any thread) makes it, returning a `shared_ptr` to any program type. The first key is built at declaration,
  since setup may compile. Later keys are built on a programs lane: a `SerializedTaskPump` on DCLF's executor, streaming class,
  because builds compile synchronously and must not take a preparation thread a compile may need. Every key built is kept. A failed
  build after the first is fatal.
- **The revision waits for a program before it starts, not inside it.** `Revision.cpp:StartPreparation` asks
  `Declarations::ProgramsReady(newest values)` first; while a build runs, no revision starts and frames keep the adopted one. That way
  a client's key or make reads the new program at the revision's start (`DeclaredProgram::For(inputs)`), and its stage builds hold
  it, so values, program and builds still adopt together. An in-revision wait would have the client registered before its program
  exists. The cost: a compile also delays unrelated inputs that arrive meanwhile (latest wins, as always). The frame reads
  `Revision::Adopted().Program(p)`, found from the adopted values at each Begin (`NoteBegin`).
- **A build must not read State.** `ProgramBuild::WithCommonDefines` reads State's defines, so SSS resolves them at setup and passes
  them with `commonDefines = false`. The same permutation as before, so the shader store hits.
- **The sample count's key is `ScaleSampleCount(SampleCount, Util::ConvertToDynamic(DisplaySize))`**, declared on `DisplaySize`,
  `DynamicRatios` and `SampleCount`. `ConvertToDynamic` reads the engine's ratio and lock live at the revision's start, as
  `GetScaledSampleCount` does on D3D11 (Upscaling sets the lock, which returns the full size). Only the program changes with it.
  Setup no longer builds every count; the others build on the lane.
- **`Enable` off keeps the client with the clear only.** Readers `Load` t45 by pixel, so a one-texel output or no clear would read
  zero, a full shadow. The raymarch pass is in the client only while enabled.
- **The output is declared on `DisplaySize`** (was the shadow mask's size), published once. Dispatches take the bound extent at commit
  (`BuildDispatches(size)`), the D3D11 path the engine's screen size.
- **The depth source is in the client's key**: Terrain Blending's published blended depth while its graph path is up and the
  revision's `TerrainBlending.Enabled` is on, else the z-prepass copy. So it switches in the revision that switches Terrain Blending's
  blend. The z-prepass import and Terrain Blending's graph state are observed at Prepass, with `Invalidate` on a change.
- **`BendSettings` is split** (`GpuBendSettings`, 32 bytes, in `RaymarchCB`). `ProjectSizeSettings` is gone.

**Decided by the agent (step 6.7, 2026-11-19), to confirm:**
- **One new frame variable, `EngineImageSpace`** (`Vars::ImageSpaceShape`: luminance levels, SAO levels, the displacement and flow copy
  widths, the flow map's identity, and how many covered effects and blend states the engine has made). The engine raises no event
  for any of these, so ImageSpace writes it at Setup and at each Begin (`OnFrameBegin`, after the adoption point). Only a change is
  an event, as with `HdrOutputTarget` and `DynamicRatios`. Additive in `Variables.{h,cpp}` and one instantiation in `Declare.cpp`.
- **The retry every 120 frames became events.** A pass left out at a build waited for one of three things: the engine making its
  effect, the engine filling its blend table, or a feature publishing an output. The first two are counts in `EngineImageSpace`, so
  making them registers the clients again. The third already rebuilds every graph at the publish, which makes the pass then.
  The 300-frame grace in `TakeChainStep` and the `pending` flags became `Lagging()`: the registration was built for other values
  than the newest written or the adopted ones.
- **Kept means History.** Bloom, the TAA pair, the adapted pair and the SAO textures are declared at Setup on `DisplaySize` (the
  adapted pair also on `EngineImageSpace`, since its size follows the last luminance level). Their descriptions read the engine
  target they replace: the engine makes its targets once a session with the display size (Q4), so a native swap chain resize finds
  the same extent and makes no version. Transients (scratches, swaps, downsamples) are made at each registration through
  `Frame::Resources`, which is now transient only (no `RequestSize`, no `fresh`).
- **One-shot clears of a kept texture's first version stay.** A declared twin's first version is not born cleared (only later
  versions are), so the first registration clears it once (`ClearFirstVersion`). Later registrations clear nothing.
- **The adapted pair is published once at Setup**, one texel like the first level while the levels have no pair. A level count the
  pair cannot follow logs an error and builds no chain clients; it never happens with the engine's targets.
- **One declaration per client, one build per group.** Each step group (world, chain, start, composites) declares each of its clients
  with the group's key. The key function builds the group's registrations once per key and gives each client its own; an empty client
  is no client. The face tint is the composites' third client, so the revision waits for its start stage build too.
- **The chain's final target comes from the key** (`HdrOutputTarget`), not the live `PostChainTarget()`, so a registration and the
  imports it names match the revision that switches it in.
- **The layout switches when the registration is built, not when it is adopted**, as before (the hooks read the new entries while the
  old build runs until the revision adopts). Entries rarely move (HDR and ratio changes keep them), so this was left. Versioning the
  layout with the registration (`ClientDesc::generation`, `FrameAnchors::HeldNow`) is the fix if a test shows wrong draws in that window.
- **`CheckImports` is deleted without a replacement.** Engine targets are made once a session (Q4) and their imports are slots;
  `EndSet` raises `DisplaySize`, which every step client follows. The flow map's republish moved from `CheckFlowMap` into the start
  registration, driven by the flow map's identity in `EngineImageSpace`; it still rebuilds every graph once (step 9 makes it a
  declared import).
- **Missing engine targets and texture failures at a registration are fatal**, not a later retry.

**Decided by the agent (step 6.13, 2026-11-19), to confirm:**
- **The readers of the cache follow a new slot count one revision later.** The revision that adopts a new slot count holds LLF's
  own stage build. The new texture is published under the old id when that revision is adopted, so the readers' graphs (the fog's
  Prepass stage, and DCLF's while it is on) are built then, staged, and switch in with a following revision. Until then they read
  the old texture, which nothing writes any more. A new slot count forgets every slice assignment, so no light points into it then.
  Making the readers members of the same revision needs the closure of a published id (who declares it) in the revision, which
  nothing gives yet. Publishing at the start of the revision instead would make the fog's export check (`IsExportOf`) fail and
  send the fog to D3D11 for the frames in between.
- **Slice counts are not part of `DeclareTexture2D`.** It stays extent-only. LLF makes one declaration per slice count, in the
  client's make, at the start of a revision on the render thread. Its first extent is made from the newest values, and the next
  revision corrects it if they differ. `ORGLocalShadows::Adopted` reads the bound extent, and is empty when that extent is not a
  power-of-two fraction of the engine's.
- **The data buffer is sized for the most slots**, so it never changes (the trigger is removed). 7 KB.
- **No D3D11 fall-back at runtime.** Once the graph holds the cache, it does for the session. The graph path needs an engine shadow
  map array (more than one slice): without one it has no cache, where D3D11 used to take it.
- **New test switch `CS_LLF_LOCAL_SHADOWS`.** The hooks install only at startup, and cs-test cannot write saved settings before
  launch. The native modes need DCLF, which blocks local shadows, so only guest and plain D3D11 run the cache.
- **Before the first adoption of the client, the frame has no cache.** The adopted call sets it, so the engine never binds a cache
  no copy wrote.

**Decided by the agent (step 6.16, 2026-11-19), to confirm:**
- **IBL's client does not depend on `EnableIBL` or `UseStaticIBL`.** The D3D11 path projects the SH whatever `EnableIBL` says,
  and with Effects 11 managing IBL the shaders turn IBL on with `EnableIBL` off (`GetCommonBufferData`). `UseStaticIBL` is a shader
  constant only. Both are ported to `Setting`; `EnableIBL` is a dependency of the fog's key. The client depends on `DALCMode`
  instead, which decides the environment pass.
- **Dynamic Cubemaps' published cubemap is read at key evaluation, not as a variable.** It is published once at its setup. Its
  mid-session D3D11 fallback (`ORGDynamicCubemaps::Fail`) withdraws the id outside a revision, which a built IBL graph then lacks;
  6.18 should make that failure fatal.
- **The runtime going down keeps IBL's D3D11 return** (`ORGIBL::Fail`), without unregistering or withdrawing: the state is kept,
  since the declaration's functions hold it.
- **The scene switch stays per-frame** (the client's `active`, and a constant for the fog). It is engine menu state, not a
  setting, and a gate keeps loading screens from rebuilding.

**Decided by the agent (step 6.17, 2026-11-19), to confirm:**
- **A reset is a new version, as the plan says, plus gated clears.** Born zero is not Skylighting's reset state, so the clears stay,
  as gated passes on the version's first run. A gate-only reset (no new version) would give the same picture without a revision;
  the version costs about 120 MB of prepared volumes per reset, briefly twice that during preparation, at every loading screen.
  Measure it with the VRAM census on the RX 6600 before keeping it.
- **`RenewFn` in Declare.h and a renew serial in `Revision.cpp`'s wish path.** The smallest change that makes a same-extent
  version; the rest of the version path (plan, prepare, adopt, `FirstFrame`) is unchanged.
- **The reset count is a feature `Setting`, not a frame variable,** to keep `Frame/Variables.{h,cpp}` out of this migration.
- **The reset is detected from the adopted count as well as `FirstFrame`.** `Update` runs only in exteriors, so a version adopted in
  an interior would miss its one `FirstFrame`. The volumes read zero until the next exterior update clears them.
- **The client exists from the first frame whose imports resolve**, about one revision after setup, not from Setup. Readers see
  zero volumes until then.
- **"The frame's graph holds the client" is a commit having run**, in place of `FrameAnchors::Scheduled`, since an adopted client
  stays in the graph and only its key changes.

**Decided by the agent (step 6.14, 2026-11-19), to confirm:**
- **The frame's method is the adopted `UpscaleMethod`, `None` before the first adoption.** `Revision::Adopted().Get` would read
  the newest written value before then, which could ask for an evaluate whose client no build holds yet. A frame or two at
  startup run without upscaling.
- **The hudless colour is three textures, not one with a format change.** `DeclareTexture2D` allows extent changes only, and a
  new slot needs staged builds of its readers that Declare.cpp does not do yet. Only the chosen one is display size.
- **`PresenterFormat` is a new frame variable.** Nothing carried the presenter's format. It is written from the presenter's
  surface each frame, as `HdrOutputTarget` is.
- **The upscaled texture also depends on `frameGeneration`,** since guest DLSS-G evaluates into it without an upscaler. That
  evaluate now skips while the adopted texture is smaller than the display.
- **Scheduled gates kept.** The evaluate's copy gate covers the post stage's keyed build (the evaluate client's `declared`
  follows `post.upscaler`), which ORG switches on its own, so a revision cannot promise it. The hudless and inputs gates cover
  registrations made from observed engine imports. Both go with step 9b and 7a.
- **The ORG failure path stays.** `ORGUpscaling::Failed` still drains the ring once and returns to D3D11 (`Upscaling.cpp`,
  `BeginRenderFrame`). It is a fallback, against rule 2.2, and not a resize drain; making it fatal is a separate change.
- **No `DeclareProgram` used.** The depth upscale reloads its programs on the render thread when the engine's target formats
  change, as before. It needs `DeclareProgram` (6.4) on the engine target formats once those are variables (step 9).
- **Builder failures in a client's make are fatal now** (Declare.cpp: "false is fatal"). They returned the feature to D3D11
  before.

**Decided by the agent (step 6.21, Atmosphere and Fog, 2026-11-19), to confirm:**
- **The composite does not follow ViewAtmosphere's `enabled`.** On D3D11 `CompositeEnabled` never reads it, and section 6.4 lists it
  for the composite. Only the atlas depends on it, through the shared volume. Changing this would change D3D11 parity.
- **The atlas is one texel while the shared volume is off**, and it is always off on the graph today (`Impl::sharedVolumePasses`,
  set once the inject and integrate passes exist). Sizing it from the settings with no writer would cost up to 128 MB for nothing.
- **A removal switches `PostWaterComposite` off at the revision's start, not at its adoption.** `Declarations::Evaluate`
  unregisters at once, which closes the gate, so the composite stops then. The flag follows it in the same frame. A deferred
  removal would need the declaration API to keep an empty key's registration until adoption.
- **`matchesById` on the composite.** Without it, a registration for new inputs leaves the stage running the old one without its
  commit, so frames between registration and adoption would have the flag on and no fog. Scaleform uses it for the same reason.
- **The composite's program stays on `ProgramCache`**, not `DeclareProgram`: its defines are fixed for the session, and only
  `ClearShaderCache` builds it again. The key carries the program, so a rebuild is a new registration with the next revision.
- **Physical Sky's `enabled` is observed, not a variable**, until 6.22 ports it. Then it joins the composite's dependencies.

**Decided by the agent (step 6.6, Screen Space GI, 2026-11-19), to confirm:**
- **No texture depends on `ResolutionMode`.** Every texture keeps the display size, as before (the chain uses the top-left part below
  full resolution, and the shaders' `TexDim` is the full size). The mode swaps which texture of each output pair is the history, so
  a mode change is a new client only, and each composite input keeps one slot. The cost: at full resolution the partners are
  History, not aliased scratch, about 14 bytes a pixel more (22 with the specular pair), about 115 to 180 MB at 4K. Below full
  resolution the memory is as before.
- **The history reset is a gated clear, not only `FirstFrame`.** `FirstFrame` covers new versions. A new configuration (which makes no
  version), the first version (not born cleared, and its client may come a revision later) and the loading-screen reset need a
  clear too, so the commit opens one gate entry for all of them. New engine inputs keep the history, as before.
- **The composite's inputs are always the published outputs on the graph path.** Off, they are one zero texel each (exported
  versions are zeroed at adoption, and the clear runs too), which reads as no SSGI. So the composite's registration no longer follows
  the toggle. Its t11-t13 choice reads the adopted specular setting.
- **Programs build on demand.** The setup prebuild of all 24 define sets is gone. A define set used for the first time compiles on
  the programs lane before its revision starts, so the change shows a few frames later. Shader reload does not rebuild the graph
  programs (step 7c).
- **Failures after Setup are fatal.** A missing input at Prepass or a client that cannot be made ends the process; the fall-back to
  D3D11 is gone.
- **The preset test step is a feature command** (`ssgi-preset`, registered in `ScreenSpaceGI::PostPostLoad`), since cs-test has no
  menu click. It calls the same `ApplyPreset` as the buttons.
- **`cs.ssgi.reset` is removed from `RecipeTable.h`.**

**Decided by the user (2026-11-18):**
- **Mid-session target recreation (Q4).** A mid-session `Create` of the engine's targets stays a fatal error. Step 9a does not build
  an event path for it. Revisit only when something is found that recreates them. Announced changes that do occur (the swap chain's
  `ResizeBuffers`, colour-space changes) still go through events.
- **VRAM cost of decision 6a.** Accepted for now. Step 8 still records the measured number in the dated doc, but it does not gate
  the step and no default changes because of it.

---

## 10. How to work in this repo

- **Worktree:** `C:\Users\matth\source\repos\SARP\external_refs\org-port-worktrees\test-runner`. Other agents may edit it at the same
  time; check `git status` before you start and touch only your step's files.
- **Branches:** CS `org-native/phases` on `panthuncia/skyrim-community-shaders`. Submodules `extern/OpenRenderGraph`, `extern/BasicRHI`,
  `extern/SceneWriterFramework` on `dev` of the `panthuncia` remotes. `extern/ORGModuleServices` on `org-rebuild` of
  `panthuncia/ORGModuleServices`; its checkout is detached two commits ahead of the local branch, so check whether those commits are on
  the remote before you commit on top. Commit in the submodule first, push, then bump it in CS.
- **Build:** inside a vcvars64 shell, `cmake --build --preset=Perf-Fast`. `BuildRelease.bat` fails at DXVK setup. Incremental builds
  take 2 to 5 minutes, up to 15 when a widely included header changes (`FrameAnchors.h` class). Keep new headers narrow
  (`Frame/Variables.h`, `Frame/Declare.h`, `Utils/Setting.h`); do not include `Frame/FrameDescription.h` or `Frame/RecipeTable.h`
  from widely included headers; give file-local names specific names (unity batches); check changed files with
  `-DSC_UNITY_BUILD=OFF` before pushing.
- **ORG and OMS unit tests:** build their test targets from the submodule (see each `CMakeLists.txt` and `tests/`); run the new tests
  you add and the existing suites in the same folder.
- **Stage:** `python tools/cs-test/cs_test.py stage . --name nfg --preset Perf-Fast`.
- **Runs:** only through `tools/cs-test`, in the background (`CS_TEST_BACKGROUND=1`), one launch per shell call, e.g.
  `CS_TEST_BACKGROUND=1 python tools/cs-test/cs_test.py run native-vulkan+resize --stage nfg --label step4`. Never raise a timeout to
  make a run pass; find why it is slow. `base` already sets `DXVK_DEBUG=hang` for the RX 6600. Never arm RGD crash capture around a
  run (it locked the machine once). About 15 launches per agent session.
- **MO2 test instance** `C:\Modding\MO2`: the profile must keep `+CS AB nfg`, `+Start On Save` and `*TerrainHelper.esp`. Do not
  restore a whole modlist backup.
- **Shaders:** do not edit headers under `Common/` without planning a shader-store warm-up (every shader recompiles). Never write an
  early `return`/`continue`/`break`/`discard` behind an `||` of equality tests (AMD RDNA2 miscompile; `AGENTS.md`). Display-size
  sizing in step 6 changes feature shaders; keep those edits in the feature's own shader files where possible.
- **Commits:** conventional (`feat(org):`, `fix(org):`, `refactor(org):`, `docs(org):`), title at most 50 characters, body wrapped at
  72, no attribution lines. Pick the type for its release impact: most of this is `refactor`.
- **Comments:** one line, two at most, plain language, only the why. No `:` `;` or chained commas in comments. No TODO or FIXME.
  ORG and OMS follow `extern/OpenRenderGraph/AGENTS.md` (short sentences, lists for sequences, Doxygen tags kept).
- **Docs of record:** a dated section per step in `docs/development/org-native-frame-graph.md` (what changed, runs with their folder
  names, launches used), and `docs/development/parity/status.md` for user-visible gaps opened or closed (the declared view mismatch,
  HDR black frames).
- **Tap errors are fatal** in development builds (`CS_ORG_MISMATCH_FATAL`). A run that stops on one is a failed run; read the line,
  it names the segment and the reason.

---

## 11. Additions from later findings (2026-11-18)

These come from the investigation of a device loss and of wrong colours on the HDR switch, recorded in
[xess-tdr-hdr-colours.md](xess-tdr-hdr-colours.md). The wrong colours are in this plan's class: the parts of one change switch on
different frames. The user asked for such findings to be added here and not fixed one by one. Fold each item into the step it names
when that step is implemented.

The plan already covers: HDR as variables `HdrEnabled`/`HdrOutputTarget` (5b, line ~446), 6.15 (HDR Display), 9a ("HDR swap chain
colour space and format: an `HdrEnabled`/`SwapChainFormat` revision. The old chain draws until the new one is adopted"), 7.7 Native,
and the `hdr` test "no black frame in a shot taken 2 frames after the switch" (step 9). It does **not** mention upscaler SDK contexts
anywhere, and its HDR text is about black frames, not about the colour space being applied ahead of the content.

Text to add:

**4.1 Step 9a, after the "HDR swap chain colour space and format" bullet (new sub-bullet):**

> - The colour space call itself moves into adoption. Today `HDRDisplay.cpp:BeginRenderFrame` latches `hdrEnabledForFrame` from the
>   live setting and calls `UpdateSwapChainColorSpace` (native: `NativeDevice.cpp:SetSwapChainColorSpace`) on the same frame, while the
>   HDR output anchor attaches about 2 frames later and the post stage's build 9-10 frames later (log of 2026-11-18, frames 1836-1846).
>   Frames in between present PQ space with SDR content, or the reverse: "strange colours" on the switch, black frames on the way back.
>   End state: `SetSwapChainColorSpace` runs inside `AdoptLatest`, for the revision whose post chain, HDR output client, LdrScratch
>   format and ImGui target format were prepared for that `HdrEnabled` value. `hdrEnabledForFrame` is `Adopted().Get(HdrEnabled)`,
>   never the live setting. The drain it does today stays (a present queued on the old space finishes first); it is the only wait in
>   adoption and is allowed because the swap chain is reconfigured. If that wait breaks "the render thread never waits" (2.2), the
>   alternative is to switch the colour space at the first present of the adopted revision on the presenter thread.
> - Guest: `OwnedFrame.cpp:ApplyColorSpace` (at present) gets the same rule: it applies the colour space of the revision whose content
>   it presents.

**4.2 Step 9, Test (extend the `hdr` acceptance):** a black-frame check is not enough; wrong colours are not black.

> New cs-test config `hdr-frames`: with the menu closed, `hdr 1`, then shots at frames 1, 2, 3, 5, 10 and 60 after the step; then
> `hdr 0` and the same shots. Acceptance: every shot after `hdr 1` decodes (PQ) to within the noise of the frame-60 shot (mean and
> 99th percentile luminance, and mean chroma), and every shot after `hdr 0` matches its frame-60 SDR shot. Needs shots that copy the
> frame as presented, without waiting for a build to switch in (today's `shot` may wait, which would hide the bad frames: check
> `Testing` before relying on it).

**4.3 New sub-section 7.11 "SDK contexts and frames in flight" (and a row in step 5b's table):**

> Upscaler SDKs keep their own GPU objects (XeSS context, FFX context, NGX feature) whose shape depends on variables: `UpscaleMethod`,
> the quality mode, `DisplaySize`, and for FSR `RenderSize` growth and the HDR flag. They are bound state like a texture version, but
> they live inside Streamline plugins, so ORG cannot version them.
> - Rule: an SDK object replaced by an options change is retired on the retirement clock, after the last submitted frame that recorded
>   an evaluate with it. It is never destroyed at the next evaluate. Today both CS plugins break this
>   (`sl.xess/xessEntry.cpp:ensureContext`, `sl.fsr/fsrEntry.cpp:ensureUpscaleContext`); that is a plain bug to fix first (see
>   `_notes/xess-tdr-hdr-colours.md` section 5), not something the revision lane fixes by itself.
> - The SDK options for a frame (`slXeSSSetOptions`/`slFSRSetOptions`/`slDLSSSetOptions`: mode, output size) come from the adopted
>   revision, not the live settings (`Streamline.cpp:EvaluateXeSS` reads `settings.qualityMode` per call today). The new context is
>   then created on the first frame of the revision whose render size and post chain match it.
> - Better (removes the trigger): prepare the new context on the revision lane, as a fragment, so creation and
>   `xessD3D12BuildPipelines` (tens of ms) do not run on the recording thread. That needs a plugin entry point to prepare a context
>   for given options without evaluating. Decide in step 6.14.
> - Decision 6a interacts: with render-size textures at display size, a quality change is constants-only for CS, but XeSS still needs
>   a new context per quality mode (its input range is per preset **(inferred; check `xessGetInputResolution` min/max per preset)**).
>   If one preset's input range covers every ratio CS offers, one context per output size would remove the trigger.

Step 5b table, new row:

> | `UpscalerOptions` | method, quality mode, output size, HDR flag | `Upscaling` settings; read by `Streamline.cpp:Evaluate*` from `Adopted()` |

**4.4 Step 6.14 (Upscaling) and step 8 tests:**

> New cs-test config `xess-quality` (native D3D12, native Vulkan, guest): `upscale xess`, 120 frames, then `setting
> Upscaling.qualityMode=` 3, 1, 3, 2, 1, 4, 1, a few frames apart (also two on consecutive frames), shots after each. Same for `fsr`
> (the growth direction is the one that recreates FSR's context). Run with DRED (`DXVK_DEBUG=hang` does this in cs-test) and once
> with `org.validation=true`: the D3D12 debug layer reports objects deleted while still in use. Acceptance: no device loss, no debug
> layer "deleted while still in use" message, shots match. Step 8's `dynres` config should include XeSS and FSR, not only the
> default method.
