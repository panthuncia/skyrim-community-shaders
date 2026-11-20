# ORG integration: cleanup and refactor plan

Status: approved, 2026-11-20. The user answered decisions D-1 to D-5 on 2026-11-20 (see
"Decisions" near the end); the sections below already follow those answers. No work has started.
A new session should read "Handoff" at the end first.

Based on a read-only survey of `integ/hard-ports` at `b1b39842d`, after the frame revision plan
finished. The survey notes, with more file:line detail than this plan, are in
[org-cleanup/surveys/](org-cleanup/surveys/).

The survey had four parts: the frame and revision core, the feature ports, the engine-side
ports, and the DCLF integration. This plan merges them. File and line references are from
that commit.

Section A lists weaknesses, each with a direction and a size. Section B lists things that
several ports built separately and that should become shared subsystems. Section C lists
code to delete. Section D gives an order of work.

Sizes: S is up to a day of agent work, M is a few days, L is a week or more, or touches
most ports.

Tags: **[known]** means the frame revision plan or `director-decisions-2026-11.md` already
records it; **[new]** means it came out of this survey.

## Rules this plan applies

The standing rules are unchanged. This plan is mostly about removing the places that still
break them.

- Errors are fatal. There are no waits, retries or fallbacks, and graph mode has no D3D11
  fallback.
- The render thread never waits. Builds, compiles and image creation happen off it.
- Changes are published at frame boundaries, through the revision. Nothing changes the
  frame's shape mid-frame.
- What a frame draws comes from declarations and variables, not from watching engine state.

## A. Weaknesses

### A1. CS does not own the engine's render targets [known in part; direction new]

CS already makes the memory. `EngineTargets` detours the engine's four create functions
(`EngineTargets.cpp:1055-1058`). ORG makes each image, and the engine's fields hold tripwire
handles over it. What CS does not own is *when* a set is made and *what shape* it has. The
engine's `BSShaderRenderTargets::Create` decides both.

What goes wrong:

- **A second target set is fatal.** That includes a window resize, and dragging the window
  frame, because the size lock in `WndProc` is skipped while the user drags
  (`Hooks.cpp:1094`, `EngineTargets.cpp:1061`).
- **Ports watch the targets.** About 19 port files keep `Observed` structs of engine target
  pointers and compare them every frame, then call `Invalidate`. Examples: kMAIN/kMAIN_COPY
  in the post chain, the z-prepass copy in Screen Space Shadows, shadow maps in five
  features, motion and depth in Upscaling, the back buffer in HDR. Keys hold raw pointers.
  An input that changed every frame would starve adoption.
- **DCLF reads target formats from engine state.** Output formats come from the bound
  output-merger state (`MainViewPoints.cpp:58-71`). The shadow format is latched from a view
  (`MaterialLookups.cpp:313-324`). DCLF then compares those observations every frame
  (`SameRevisionInputs`, about 30 fields, `Coverage.cpp:77-90`).
- **Features change the fields.** HDR remakes and restores targets and redirects
  kFRAMEBUFFER's RTV to its UI texture (`HDRDisplay.cpp:1556,1619`). Deferred calls
  `Recreate` (`Deferred.cpp:90`). Skylighting lends a slot (`Skeleton.cpp:1222`). In graph
  mode each of these is an `OutsideAdoption` tap error.
- **Some targets are still engine objects.** In guest mode, kFRAMEBUFFER is DXVK's back
  buffer and is found again on every import through `RTV->GetResource`
  (`FrameAnchors.cpp:4202`). HDR's UI texture is a D3D11 texture imported as `"hdr.ui"` in
  four files. The Volumetric Lighting volumes are made inside a frame (`MakeVolume`, `:894`).
- **Lookup is linear and done many ways.** There are 176 `renderTargets[` and 98
  `depthStencils[` reads, and 105 imports, each resolved by a linear `Targets::Find`
  (`:1269`). See B4.

Direction (L): **CS owns an `EngineTargetSet` and treats each target like any declared
resource.**

1. The `Create` thunk only records the engine's properties and raises `DisplaySize` and
   the shape variables. After the first set it never makes or releases anything.
2. Each engine target is a declared texture on those variables: kMAIN, kMAIN_COPY, depth
   and z-prepass, motion, the shadow cascades and the rest. The revision lane makes new
   versions off the render thread, as for any declared texture.
3. At adoption, CS points the engine's field handles at the new versions (a new tripwire
   retarget operation) and writes the engine's size state (`BSGraphics::State`, viewports).
4. `State::Setup` and `globals::ReInit` do not run again. Features follow their
   declarations.
5. The VL volumes are declared with the set, sized from the INI.
6. A target that no declaration reads is never made, in both modes. This replaces the
   hand-kept unbacked table (`Frame/EngineTargetUse.cpp`).
7. In guest mode, the scene back buffer becomes a CS slot, as it already is in native mode.
   Present copies it into the DXVK swap chain image. HDR's UI texture becomes a declared
   target (B17). The user accepted the cost of that copy (D-2), on condition that it is
   investigated. See "Investigate: the present copy" below.
8. HDR's and Deferred's target changes become plans for the declarations. Their
   `Recreate`/`Restore` branches leave graph mode.

Blockers to solve first:

- Guest handles forward to a fixed DXVK object and cannot be retargeted. Native mode already
  has an indirection (`Rename`/`CurrentOf`). Guest needs the same: either a DXVK-side
  retarget, or engine fields that point at a CS proxy.
- Native wraps at adoption cost 19-75 ms each and wait with an immediate submission
  (decisions Q2, still open). They need to move onto the lane before targets are remade at
  runtime.

Interim fix (S), until this lands: in graph modes, also apply `SWP_NOSIZE` while the window
frame is being dragged, so the drag is not fatal.

**Investigate: the present copy (D-2).** Native mode already copies the CS back buffer slot into
the swap chain image at present (`NativeDevice.cpp:1908-1980`), and step 7 adds the same copy
in guest mode. The user wants to know why the copy is needed and whether it can go. Questions
to answer, with measurements on the RX 6600 at 1080p and 4K:

- What does the copy cost on the GPU, and does it add latency to present?
- Why is a copy needed? Likely reasons to check:
  - Swap chain images are acquired late, so the frame cannot name its image when it is
    recorded.
  - A recording that replays must not name a per-frame swap chain image.
  - Formats and usage flags may differ (an sRGB view, storage usage, HDR formats).
  - The DXVK swap chain owns its images, and frame generation (DLSS-G, FSR-FG) takes its own
    copy of them.
- Can the last pass that writes the back buffer write straight into the acquired image? For
  example, it could be a separate late-bound step that is not part of the replayed recording,
  or the slot's version could be bound to the acquired image at present.
- Do the answers differ between guest, native Vulkan and native D3D12?

Write the findings into this section. Remove the copy only if a mode can do it without
breaking replayable recordings or frame generation.

Once this lands, the `Observed` sites, DCLF's `SetTargetFormats`, `SetShadowInputs` and
`RecreateShadowSet`, and `targetsGeneration` are deleted. Ports name targets by id.
`DrawPipelines` is built from declared formats at setup.

### A2. Watching state by hand and calling `Invalidate` is the normal pattern [known as interim; how widespread it is is new]

There are about 40 `client.Invalidate()` / `programs.Invalidate()` calls in about 30 files.
`Declare.h:217-221` calls `Invalidate` "an interim … until its feature migrates". Every
migration has landed, and it is still used everywhere.

Key functions read `this->observed` instead of their `Inputs`, so a key is not a function of
the revision's values. `Declare.h:252-254` warns that such a key may never match the adopted
values. Some of these paths draw with D3D11 until adoption (`ORGHDRDisplay.cpp:522`).

The same idea appears outside the ports:

- DCLF's `SameBuild` and `SameRevisionInputs`.
- The wanted/made serial pairs in Scaleform, the screenshot capture, frame generation inputs
  and glints.
- ImageSpace polls `EngineImageSpace` at every Begin, including a raw flow-map pointer and
  counts of the effects the engine's manager has made (`ImageSpace.cpp:1533-1572`).

Direction (L, mostly done by A1):

- Engine targets: removed by A1.
- Other observed state becomes a CS-owned typed variable, set where the hook sees it
  (`Frame::Observed<T>`, B2). Keys read only `Inputs`.
- Engine shape facts, such as ImageSpace's effect counts, become events from the engine's
  creation hooks, not values polled every frame.
- Only truly external inputs stay observed: the back buffer from the swap chain, and LUT
  files.
- Then `Invalidate` is deleted.

### A3. Graph mode still falls back to D3D11, and failures are not fatal [known in part; the general case is new]

**Failures after setup.** These log "returns to D3D11", then call `FrameAnchors::Unregister`
mid-frame:

- `ORGTerrainBlending.cpp:428` (`Fail`, called each frame from `Observe`)
- `ORGHDRDisplay.cpp:390`
- `ORGScreenSpaceShadows.cpp:423-435` (`Stop`)
- `ORGIBL.cpp:281,302`
- `ORGUpscaling.cpp:511` (`MarkFailed`) and `Upscaling.cpp:733`

Since the late-changes step, that call is a `LateChange` tap error. In native mode the D3D11
path is a tripwire no-op (`Tripwire.h:24`), so it was never a fallback there. Meanwhile, ten
other ports already use a fatal helper, so the policy is inconsistent.

**Failures at setup.** About 60 setup returns log "stays on D3D11". So do ImageSpace `off()`
(`ImageSpace.cpp:2079`), EngineTargets' "made on D3D11 again" (`:648`), ImGui `FallBack`
(`ImGuiRenderer.cpp:281`), ten sites in Skeleton, OcclusionQueries, EngineBuffers and Bink.
In native mode each one silently drops the feature. In guest mode it splits one feature over
two paths.

**Runtime faults.** `RenderGraphRuntime.cpp:3471` says "callers fall back to D3D11". Every
port checks `IsActive()` each frame and handles a fault in its own way: fatal, D3D11, or
silently off.

**Frames that still run on D3D11** [known: items 43, 46, 66; the rest new]:

- Exponential Height Fog has 12 per-frame reasons to run on D3D11
  (`ORGExponentialHeightFog.cpp:1087-1205`).
- Any `Update` hold sends the whole post chain to D3D11. Holds come from Color Grading's
  preview or save, an unloaded LUT, or Lens Flare and Physical Glare debug switches. So do
  "no stage held", `StagePassed`, and the frames before the first plan.
- FX and Sun Rays: no stage, missing inputs, a reload after the plan.
- HDR, Terrain Blending and Subsurface Scattering draw on D3D11 until their first
  registration or commit.
- Cloud Shadows flushes queued layer operations to D3D11 (`CloudShadows.cpp:69-104`). Its
  occlusion copy goes to D3D11 when the queue refuses it (`:309`).
- Engine non-draw work is not gated: copies, clears, maps, updates and queries
  [known for Skeleton, item 66]. Skeleton returns clears and copies to D3D11
  (`Skeleton.cpp:1212,1513`). Bink falls back to the engine's draw. Occlusion queries
  outside a bracket stay on D3D11. `KeptWrites` maps DXVK buffers. In native mode these go
  through the immediate context, which host-waits on the render thread
  (`NativeDevice.cpp:662,1023,3680,3724,3840`).
- The flush latch only logs a withheld engine draw (`Flush.cpp:682-716`).

**DCLF coverage exemptions** [new]:

- "An uncovered frame is fatal" applies only to claimed epochs, after `NotDrawingYet`, and
  outside load screens (`FrameDriver.cpp:212-235`). Gated epochs are exempt
  (`Coverage.cpp:172-177`).
- The engine still draws in these cases:
  - forward and first person without forward batches;
  - a CPU site that is not live;
  - occlusion maps;
  - the colour epoch when the target capture fails (`MainViewPoints.cpp:84-91`);
  - a family whose client is not registered yet;
  - per-draw `TryLatchDraw` refusals (seven files).
- After `kSceneIdleFrames`, DCLF draws its old scene on old frame versions.
- `CheckSceneVersions` is only a counted warning.

**Rule violations in release builds.** `MismatchFatal()` is false under
`CS_RELEASE_PROFILE`, so the error is logged once and the work is skipped, which is a
fallback (`FrameDriver.cpp:413-450`). The census runs in development builds only. Every tap
error prints "native mode: the frame did not match its schedule", in guest mode too.

**Error policy differs.** A staged refusal at a DCLF scene adoption drops the scene and asks
again. The same refusal is fatal for a frame revision.

Direction (L overall; the pieces are S to M):

1. One helper, `RenderGraph::PortFatal(feature, why)` (B7). In graph mode, every failure in
   setup, at runtime, or in a port is fatal through it. A runtime fault calls
   `report_and_fail`. `IsActive` means "set up" only, and the per-frame checks go.
2. Delete the D3D11 recovery paths (section C).
3. A frame kind that has no stage gets one. The first adoption happens before the first
   world frame, because declarations are evaluated at setup's build point. That removes
   "until first registration".
4. Post-chain holds and debug switches become declared program or key choices, not holds.
5. A missing input is fatal.
6. Coverage is declared per epoch in the recipe. An owned epoch is either covered or a tap
   error. Engine fallbacks become named states, switched only at the adoption point.
   Per-draw latch refusals move to classification time, or are fatal. `CheckSceneVersions`
   becomes a tap error.
7. New tap errors: `EngineDraw` (a withheld engine draw) and the census.
8. Engine constant-buffer pools become ORG buffers, as `EngineBuffers` did for vertex and
   index buffers. Then `KeptWrites` is deleted.
9. Rule violations (D-5): `OutsideAdoption`, `LateChange` and the other tap errors stay
   fatal in development builds only. In release builds (`CS_RELEASE_PROFILE`), the cheap
   counters stay on, and the first violation of each kind:
   - writes the full error to the log, with file:line and the frame;
   - raises a warning that the DCLF menu shows until the game exits. The warning names the
     error kind and tells the player to send their logs to the developers. It names the log
     files and where they are (`CommunityShaders.log` and the DCLF log, in the SKSE log
     folder);
   - keeps the game running, as it does today. Whether the affected work is still skipped is
     decided in the change; the menu warning is what matters.
   Each tap error kind gets its own message, naming the actual mode (guest, native Vulkan or
   native D3D12), not "native mode" for all of them. The warning text goes through i18n.
10. A staged refusal at a scene adoption is fatal.

Order matters here. Steps 1-2 and 9 can land first. The rest of the "draws on D3D11" frames
must be removed (steps 3-8) before the census can be made fatal.

### A4. The render thread still waits and compiles [known in part]

- **Shader reload** [known: item 57, Q5]. The reload waits up to 120 s inside Begin
  (`Snapshot.cpp:929-1094`). While waiting it pumps builds, submissions and scene adoption.
  - Direction (M-L): shader generation becomes a revision input (`ShaderGeneration` and
    `AdoptedShaderGeneration` already exist). The swap's revision includes DCLF's
    re-recorded scene and is adopted only when that scene is ready. Frames keep the old
    pipelines meanwhile, kept alive by the old recordings.
  - Then delete `WaitAfterPipelineSwap`, `SupersedeForSceneWait` and `AdoptScene`.
- **Program builds on the calling thread** [known in part: 6.9, 7c].
  `ProgramCache::GetNow` and `Require` build or wait on the caller (`ProgramCache.h:92-130`).
  - Callers on the render thread:
    - Bink (`BinkGraph.cpp:218`);
    - Scaleform (`Passes.cpp:1435`);
    - ImageSpace at graph build (`ImageSpace.cpp:1249`, `ChainSteps.inl:687`);
    - Upscaling's copy and depth programs, compiled "at first sight" from per-frame queue
      calls (`ORGUpscaling.cpp:519-528,1157-1170`).
  - A failed build means "what needs it stays with the engine", which is a fallback.
  - FX parsing and lowering run inside the key function, and the post chain is replanned at
    every adoption.
  - Direction (M): one program service (B8). Programs with a key use `DeclareProgram`;
    fixed programs are queued as a batch at setup. Delete `GetNow` and `Require`. Planning
    moves onto the programs lane.
- **Native wraps** [known: Q2]: see A1.
- **Native resize** waits on the host with a 2000 ms timeout (`NativeDevice.cpp:2004`).
  Present has a "copies the back buffer by hand" fallback (`:1947`).
- **`PrimeStage`** sleeps 1 ms in a loop with a 10 s deadline (`FrameAnchors.cpp:4134`).
- **Physical Sky cloud maps** retry each frame and use a timer. `Submit` returns false with
  "work waits", and three generators retry next frame. Maps are pruned after
  `kStaleFrames = 600`. `EndFrame` builds a new client on the render thread
  (`ORGCloudMaps.cpp:572-696`).
  - Direction (M): each generator declares its steps once at setup, as a declared client
    keyed on its map set.
- **ImageSpace holds step groups on for 300 frames** (`kRequestFrames`), which is a timer. A
  lagging step draws nothing. Draws are matched by counting occurrences.
  - Direction (M): steps come from description fields read once at Begin. A mismatch is
    fatal. `requested[]` is deleted. See also B21.

### A5. The core frame machinery depends on DCLF [known: accepted as Q3; the weakness is new]

There are 27 DCLF references in `Revision.cpp` and 27 in `RenderGraphRuntime.cpp`.

- The revision lane and the programs lane run on `DCLF::SceneScheduler::Executor()`
  (`Revision.cpp:664-674`, `Declare.cpp:234-243`).
- Retirement uses `DCLF::FrameRetirement()`.
- The revision has DCLF-specific slots (`kRevisionScene`, `kRevisionSceneBuffers`).
- It has DCLF-specific checks and state (`IsDclfEpoch`, `heldScene`, `sceneMissesSinceInput`).
- It declares DCLF functions by hand (`Revision.cpp:47-61`).
- `FrameDriver` calls `DCLF::BuildPointAtFrameStart`.
- `DclfToggles` and `DclfPassCuts` are core variables.

This is why the graph modes require DCLF (item 45). It is also why turning DCLF off
mid-session blanks the world, and why a second scene-like producer would need more special
cases.

**Two recording machines.** DCLF's `RevisionAssembler` (`Assemble.cpp:33-260`) and the
frame's `MakeRevisionRecordings` (`FrameAnchors.cpp:2444,3488`) both request epoch
recordings, catch dropped recordings, and check build generations. Their failure rules and
starvation rules differ.

**Three DCLF build paths** [known in part: 10a]:

- `ExternalBuilder` (`BuildManager.h:32-50`).
- `ownerBuilt`, the `CS_DCLF_ASYNC_BUILDS=0` path, which builds on the render thread.
- A refused build falls back to the owner thread.

An epoch's first build is synchronous on the render thread (`GraphBuilds.cpp:142`). DCLF
also has a build lane of its own.

**Pass preparation reads the current conditions, not the build's** [known: 10c open item].
`CurrentGraphConditions()` is a thread-local, or else the attached state
(`GraphConditions.cpp:236`). It has 36 call sites, so recording a staged build reads older
conditions. The three build paths exist only because of this.

Direction (L, in this order):

1. A build carries its own immutable `BuildState`. Passes capture it when they are made.
   Delete `CurrentGraphConditions`, `BuildScope` and `SetBuildStateSource`. (M)
2. DCLF epochs become plain staged epochs, built on the `BuildManager` thread. Delete
   `ExternalBuilder`, `ownerBuilt`, `CS_DCLF_ASYNC_BUILDS` and the DCLF build lane. (M)
3. The executor and retirement clock move into CS `RenderGraph` (B9, B12). (M)
4. A generic fragment source replaces the hand-declared DCLF functions:
   `Revision::RegisterFragmentSource{versions(), take(), adopt(), drawing()}`. DCLF
   registers itself like any other client and declares its own variables. (M)
5. One per-epoch recording service in CS `RenderGraph` replaces both machines (B10). The
   scene becomes ordinary members of the frame revision. (L)

Graph mode keeps requiring DCLF (D-3). So the aim of A5 is a clean structure, not making
DCLF optional: the core no longer names DCLF, and DCLF uses the same interfaces as any other
client.

Turning DCLF off mid-session in graph mode stays unsupported; today it blanks the world. The
director decided, to confirm: in graph mode the DCLF on/off control is locked in the menu, and a
saved change applies at the next launch. That makes the blank world impossible to reach.

### A6. Mid-frame changes are not fully caught [new]

- **Other threads.** The late-change check counts the render thread only
  (`BoundaryCheck.h:15`). `Builds::Request`, `RequestAll`, `RequestFor` and `Change` accept
  any thread, so requests from the menu, cs-test or a builder thread escape the check.
- **`FrameAnchors::Run`.** It still calls `PollEpochFailures`, which drops clients. It also
  calls `Service`, which holds clients that wait for the shared constants and calls
  `Builds::Request` mid-frame (`FrameAnchors.cpp:2023-2047,3814-3817`).
- **Mid-frame declarations.** `DeclareVanillaVolume` is called mid-frame
  (`ORGVolumetricLighting.cpp:458`). `Declare.cpp` has no thread checks and no locks.
- **Three ideas of "render thread".** `IsRenderOwner` (87 calls), `IsMainThread`, and the
  boundary check's "last thread to open a Scope". On a loading thread, revisions adopt there,
  but DCLF adopts only on the main thread (`Revision.cpp:1276`).
- **`Frame::Resources::Texture2D`** remakes itself when it gets a new description
  (`FrameResources.cpp:26-38`, one user).
- **Test commands.** `dclf:refresh` calls `FrameAnchors::Refresh` inside the frame. That is
  now fatal, so the stress tool is unusable.

Direction (M):

- Every rebuild request goes into one queue that is drained only at the adoption point
  (`Builds::Ask`, B11).
- The shared constants are made at setup, and the waiting-client path is deleted.
- Epoch-failure handling moves to the adoption point.
- One render-thread check, asserted in `Declare*` and `Evaluate`.
- Retire `CS_ORG_LOADING_THREAD` (D-4). Delete the switch, its registry entry and schema
  key, and the loading-thread adoption path. Then the adoption point runs only on the main
  render thread, and `Revision.cpp:1276`'s split goes.
- The VL volume is declared at setup (A1, step 5). `Frame::Resources::Texture2D` becomes a
  declaration on `DisplaySize`.

### A7. Variable and setting API gaps [new]

- **Two ways to read an adopted value.** There are 14 `Vars::AdoptedX()` getters alongside
  `Revision::Adopted().Get(Var)`. Setters are written by hand (`Variables.cpp:148-209`). The
  sets do not match: `UpscalerOptions` and `PresentationConfig` have setters but no `Var<>`;
  `Interior`, `ShadowCascades` and `EngineImageSpace` have no getter.
- **One producer per variable.** `RegisterInputProducer` fails on a second producer
  (`Revision.cpp:1252-1260`). DCLF and Upscaling register "producers" that are ready at once
  and only want a callback at adoption.
- **`Interior` has two writers**: DCLF, while it runs, and Volumetric Lighting. DCLF caches
  `adoptedInterior` itself and falls back to the engine's value.
- **Copies of adopted values.**
  - DCLF keeps four copies of its toggles: requested, active, frame, and a thread-local
    copy (`Toggles.h:76-143`). It also has a path that applies them without a revision
    (`Toggles.cpp:392-401`, `UseRevisions(false)`).
  - Features keep per-frame adopted copies for readers outside the frame:
    `TerrainBlending::frameEnabled`, `PhysicalSky::frameEnabled`,
    `ORGSunRays::AdoptedToggles`, `AdoptedSpecularGI`, and Upscaling's adopted method.
- **GPU constants read live settings, while the frame's shape uses adopted settings**
  [known for Effects11 and EHF; the general split is new].
  - `ExponentialHeightFog.cpp:102-140`, Dynamic Cubemaps, Terrain Blending, ViewAtmosphere
    and the bend settings copy fields one by one from the live struct.
  - For one frame after a change, the constants and the shape disagree.
- **Unported settings.** The `SettingsWritten` shims remain: 22 references in 10 files.
  `ProjectSizeSettings` is still overridden in GrassCollision, PostProcessing and
  TerrainShadows. `Vars::RaiseFeatureSettings` makes variables that nothing depends on [known:
  section 8].

Direction (S-M):

- One constexpr table of `Var<T>`, with a generic `Vars::Set(Var<T>, v, writer)` and
  `Vars::Adopted(Var<T>)` (B13).
- `Vars::OnAdopted(var, cb)` allows many listeners. `RegisterInputProducer` stays for real
  work done outside the frame.
- `CS::Setting<T>::Adopted()` is published at adoption into an atomic copy that any thread
  can read.
- One engine-observation sampler at Begin writes `Interior`.
- DCLF's toggles: the requested value lives in a `Setting<T>`, and the adopted value is read
  from `Vars`. Delete the direct path.
- GPU blocks are filled from adopted values through a field map (`GpuBlock`, B14).
- Port the three remaining features to `Setting<T>`. Then delete the shims and the
  FeatureSettings variables.

### A8. First contents and "was it written" [known in part: late-changes first-run gates]

New versions are born cleared on the lane. A declared texture's first version is not, and
neither is an exported one. So features built their own ways to handle first contents:

- `ClearFirstVersion` in ImageSpace.
- `FirstRun` gates in Dynamic Cubemaps, Terrain Shadows, ViewAtmosphere and Glints.
- `AdoptedRevision::FirstFrame`. It is true for a frame, not for a run (`Declare.cpp:695-712`),
  so it is missed when the pass does not run that frame.
- PhysicalGlare's `fresh` flag.
- EHF's `Written()`, which compares `weak_ptr`s.
- Physical Sky's `shadowCleared`.
- The clouds' `s_outputsWritten`.
- In Skylighting, `mapCleared`, `committed` and `resetsSeen`, with a native-only branch.
- In SSGI, `lastReset` with `queuedResetHistory`.
- HDR's `uiCleared`.
- Terrain Blending's `TerrainDepthCleared`.
- FX's "clear on each new version's first run".

Direction (M): a `Birth` policy on the declaration (B5). It is applied to every version,
including the first and exported ones, and made on the lane. `Adopted().Written(slot)` is set
by the run that writes it. `FirstFrame` becomes "the first run that binds this version".
Uploads go through `PublishTexture`/`PublishBuffer`. Then `FirstRun` and the hand-written
flags are deleted.

### A9. Too many ways to ask "will my work draw this frame?" [known in part: section 8 `Scheduled`]

- `FrameAnchors.h:275-454` has 14 overlapping queries: `Scheduled`, `ScheduledThisFrame`,
  `StagePassed`, `InStage`, `HeldNow`, `CanTap`, `TapMayLag`, `InBuiltGraph`, `StageOpen`,
  `StageAbsent`, `HeldByStage`, `NamedByStage`, `StageStepsAt` and `StagePlace`.
- `ClientDesc` has six flags that turn work on or off.
- `Scheduled` is still called in 8 places that section 8 meant to remove (Cloud Shadows,
  EngineDrawFamily, Screenshot, Upscaling ×3, DeferredClears, Scaleform).
- Several ports keep a list of their builds and match `HeldNow().generation` to find the
  build this frame holds (B3). An unknown generation is only logged, and the work is not
  drawn.

Direction (M): one `Placement Place(id, anchor)` returning `{stage, held, scheduled,
generation, lag reason}`. `DeclareClient` returns the held state directly (B3), and an
unknown generation is fatal. Merge `active` into `declared`. The `Scheduled` callers become
declared clients with a `declared` predicate.

### A10. Too many frame counters and serials [new]

- The frame driver advances its frame at Begin. The revision computes `frame + 1` before
  that, which couples them by call order (`Revision.cpp:1261`).
- `BufferRefs` advances at `SealPresented`, a different boundary.
- DCLF has its own uint32 `FrameView::frame`, which only moves while DCLF runs, and uses
  `GetFrame() + 1` in three places.
- Others: `gameFrame`, `completedGameFrame`, `framePaceValue`, ORG's host frame,
  `presentCount`, `frame % 4`, and a static `ahead` counter.
- About a dozen serials have mixed names (`generation`, `serial`, `keySerial`,
  `VersionsSerial`, `extentSerial`, `renewSerial`, `groupSerial` and others).

Direction (S-M): one read-only `FrameClock` (B12). The word "generation" is kept for
registrations only.

### A11. Errors that name the wrong cause [new]

- `Resource::Preparable` folds six causes into one bool (`GraphResources.cpp:744-768`).
  `Revision.cpp:686` reports all six as "the extent changes its shape class". A second check
  prints a different message.
- `Declare.cpp:330` quietly changes a Transient texture to Persistent. Twins that are not
  declared keep Transient.
- The shader-reload failure text still names "targets generation".
- Every tap error says "native mode", in guest mode too (A3).

Direction (S): `Resource::WhyNotPreparable(extent)` names the cause, and both call sites use
it. `DeclareTexture*` rejects a Transient lifetime with a fatal error instead of changing it.
Messages are fixed.

### A12. Published resources [known in part: decisions 4 and 44]

- `Resource::Register` silently keeps the first resource when an id is registered twice
  (`GraphResources.cpp:705`).
- `Withdraw` sends readers back to D3D11.
- Replacing a published resource asks for a structural rebuild (`PublishedResources.cpp:177`).
- Features learn about each other's graph state through D3D11 export identity: `IsExportOf`,
  `IsPublished` and `ORGX::Get().IsActive()` (9 probes in EHF, 4 in Physical Sky, 2 each in
  three more).
- Bindless indices for D3D11 views go through `DCLF::GpuTextures` from five non-DCLF
  features (B16).

Direction (S-M):

- A duplicate id is fatal.
- Published ids are declared once, at setup, and resolve to versioned slots. Publishing
  never rebuilds.
- The existence rule applies everywhere: every published id always exists, at one texel
  when it is off (B6).
- A dependent reads the owner's adopted `Setting` to learn whether it is on, never its
  runtime state.

### A13. Separate cases [mixed]

- **Light Limit Fix culling** runs its own epoch through a host extension
  (`ORGLightCulling.cpp:509,584`) [known: step 6 follow-ups]. It is not a CS stage, so it is
  not a revision member. Whether it binds the new buffers on the first frame after a resize
  was never checked. Direction (M): make it a stage client at an anchor.
- **Light Limit Fix local shadows on the graph cannot run** [known: item 45].
  `ORGLocalShadows` (645 lines) is blocked whenever DCLF is loaded (`LightLimitFix.cpp:645`),
  and graph modes require DCLF. EHF's fallback for it (`:1180`) is dead.
  - Direction (M-L, D-1): rework the cache to follow DCLF's shadow views. Today the cache
    takes over the engine's local shadow draws, which conflicts with DCLF owning them. Instead:
    - DCLF's shadow epochs render the local lights' shadow views as they do now.
    - The cache becomes a declared texture array that those views write into, or a reader of
      DCLF's published shadow slots, keyed on DCLF's shadow-view set.
    - Then remove the DCLF block at `LightLimitFix.cpp:645` and bring EHF's local-shadow
      input back.
  - The lead should first map how DCLF's shadow epochs (`ShadowEpochs.cpp`,
    `ShadowEpochPasses.cpp`) name local lights' views, and what the cache needs per light:
    slice, matrices and update rate.
  - Test with the `llf-local-shadows` and `llf-parity` configs.
- **The D3D12 path degrades DCLF through environment variables** (`Native.cpp:78-100`)
  [new]. Direction (S): use a device capability that DCLF reads at setup.
- **Presentation branches on the mode** in four places (`PresentationRevision.cpp`). A DXVK
  that does not report replacements returns serial 0, so the revision can be adopted before
  the swap chain changes [new]. Direction (S-M): one presenter contract for both modes, with
  no fallback.
- **ImGui reads the bound target** with `OMGetRenderTargets` (`ImGuiRenderer.cpp:96`)
  [known in part: 9b]. Direction: the overlay draws into a declared target (B17).
- **Open memory items** [known]: SSGI +115-180 MB at 4K, the Skylighting reset 120 MB,
  LensFlare 56 MB, and the step 8 VRAM procedure. These are measured, not refactored.

### A14. Test support is fragile, and test code ships [known in part: items 3, 47, 63]

- Shots are taken at hand-tuned frame counts after a `setting` step. A test cannot wait for
  the revision that holds its write.
- Pass and fail depend on `no_log` substrings. That has given false failures twice.
- Only the `effects11` config checks for D3D11 frames, and it cannot run on this machine.
- `TestHarness.cpp` is compiled and registered in every build. It carries generic graph
  commands, not DCLF ones: `rebuild`, `refresh`, `reload-shaders`, `nvperf`, `gpu-window`,
  `async-compute`, and switches that change runtime submission.
- There are 17 `TEMP (CS_DCLF_SET_PARITY)` markers in production structs.
- `CS_DCLF_TEST_COMMANDS` also gates Bink.
- Per-feature test hooks: `ssgi-preset`, `CS_LLF_LOCAL_SHADOWS`.
- `schema.toml` has 20 dead keys: 17 `org.<feature>` keys and 3 flush-latch switches.

Direction (M), see B23:

- A cs-test step `await = "adopted"`.
- A structured per-client "ran on graph" frame counter in the exit report, checked by
  `expect.graph_frames`. This replaces most `no_log` checks.
- An `org:` harness in `RenderGraph/Testing`, compiled out of Release. Its `refresh` goes
  through `RequestAtAdoption`.
- A generic `call` step over a feature command registry.
- Delete the set-parity TEMP code and the dead keys.

## B. Shared subsystems to build

Each item names the copies it replaces. Proposed homes: `src/RenderGraph` for frame and graph
pieces, `src/Utils` for generic helpers. Library-level pieces (OMS/ORG) are marked.

### B1. `EngineTargetSet` (A1)

CS owns the engine's targets as declared textures on `DisplaySize` and the shape variables.
Field handles are retargeted at adoption. This replaces:

- the `Observed` engine-target structs in about 19 ports;
- `TakeOver` and the remake in `BeginSet`;
- the native unbacked-target table;
- DCLF's format and shadow observation;
- HDR and Deferred `Recreate`/`Restore` in graph mode;
- `MakeVolume` inside a frame.

### B2. `Frame::Observed<T>` (A2, interim)

```cpp
Frame::Observed<VLInputs> inputs{"VL.Inputs"};
inputs.Observe(seen, "VL render");          // raises the variable only on change
auto client = DeclareClient<VLInputs>(id, DependsOn(inputs.Var()),
    [&](const Inputs& v) { return v.Get(inputs.Var()); }, make);
```

This holds observed state that has no owner yet. Keys then become functions of `Inputs`. It
replaces `Invalidate`, the `observed` members, and the wanted/made serials. Most uses go away
again once B1 lands.

### B3. Held state from `DeclareClient` (A9)

```cpp
auto c = DeclareClient<Key, State>(id, deps, key, make /* returns shared_ptr<State> */);
Held<State> h = c.Held(anchor);              // State*, or NoStage / Lagging
```

`Declare` keeps the per-key objects. An unknown generation is fatal. This replaces the build
lists and generation matching in:

- Sun Rays (`:651`), the post chain (`:1054-1080`), FX (`:1710,2147`);
- Scaleform, Skeleton and `EngineDrawFamily`;
- HDR `held`, Physical Sky's deferred step, Volumetric Clouds, ViewAtmosphere and
  Screenshot.

It also replaces `Scheduled`, the four ImageSpace `*Lagging` checks, and the wanted/made
pairs.

### B4. `Targets::Identify` (A1)

```cpp
std::optional<TargetRef> Targets::Identify(const ID3D11View*);      // {kind, index, slice, mip}
std::optional<TargetRef> Targets::Identify(const ID3D11Resource*);
Texture Targets::Slot(TargetRef);
```

These are backed by hash maps filled when targets are made. They replace:

- ImageSpace `InputOfView`/`TargetOfUav`;
- Skeleton `FindColour`/`FindDepth`;
- Scaleform `EngineTargetOf`/`EngineDepthOf`;
- the engine-depth search loop copied in Physical Sky, Volumetric Clouds, Skylighting and
  EHF;
- the linear `Targets::Find`/`FindImage`.

### B5. `Birth` policy and `Written` (A8)

Declarations take `Birth::Zero`, `Birth::Value(v)`, `Birth::Fill(fn)` or `Birth::Undefined`.
The lane applies it to every version, and `Adopted().Written(slot)` is set by the writing run.
OMS `VersionedTextureGrowth` already has the needed contents support. The copies this
replaces are listed in A8.

### B6. `ExistsWhen` and `DisplaySized` (A12)

```cpp
DeclareTexture2D(id, Frame::DisplaySized(desc), ExistsWhen(Vars::Setting(on)));
```

The one-texel rule is written by hand in about 15 places:

- EHF, Sun Rays, the Physical Sky clouds, ViewAtmosphere, SSGI, Upscaling, LLF, Terrain
  Shadows;
- five post effects.

The `DisplayOr(width ? : setupWidth)` fallback is written in nine places. `DisplaySize` is
written before the first declaration, so it is never zero, and the fallback goes.

### B7. Port skeleton, fatal helper and reason log (A3)

- A `RenderGraph::Port` base class or a `CS_ORG_PORT(Name)` macro. It replaces, across
  24-26 ports:
  - `Requested()`, which always returns `GraphMode()`;
  - `Reserve`;
  - the `Get` singleton;
  - `IsActive`;
  - the `#else` stub blocks of 20-40 lines each.
- ORG files compile only with `CS_HAS_RENDER_GRAPH`; call sites go through a null port.
- The `Requested()` functions in the 10 engine modules and ImGui fold into `Engine::On()`.
- `RenderGraph::PortFatal(feature, why)` replaces 11 `[[noreturn]] Fail*` copies.
- `Util::ReasonLog` replaces `fallbackReason`, `refusal`, `SetReason`, `Inactive` and the
  `call_once` logs.

### B8. `ProgramService` (A4)

One service on one executor, used through `DeclareProgram` for keyed programs and a setup
batch for fixed ones. It replaces `ProgramCache`'s own thread, `GetNow`/`Require`, about 70
raw `Load` calls, and the render-thread compiles in ImageSpace, Bink, Scaleform and Upscaling.

### B9. `Frame::Executor`, `LatestSlot<T>` and `MakeLane` (A5)

A CS-owned executor in `RenderGraph/Async`, possibly OMS `ServiceExecutor`. It runs the
revision, programs and build lanes. It replaces:

- DCLF's `SceneScheduler` as the core's executor, and `SceneScheduler::MakeLanePump`;
- DCLF's `LatestSlot` and its build lane;
- `Revision`'s mutex-and-optional `postedScene`;
- the separate `ProgramCache` and `BuildManager` threads. There are four threads today.

### B10. `Frame::EpochRecordings` and `StarvationWatch` (A5)

```cpp
auto fragment = Frame::EpochRecordings::Request(epoch, hostData, versions, buildGeneration);
Frame::StarvationWatch watch{"DCLF scene"};   // NoteInput(); NoteMiss(why) -> TapError::Starved
```

These replace DCLF's `PendingRecordings` and the frame's `RevisionRecordings`. They also
replace four starvation counters, each with its own reset rule:

- `kRevisionStarvedAfter`;
- `sceneMissesSinceInput`;
- `kRestageStarvedAfter`, which resets on the build serial and can fire falsely;
- `kSceneIdleFrames`.

### B11. `Builds::Ask` (A6)

```cpp
Builds::Ask({.ids = ..., .epochs = ..., .all = false, .structural = false}, "why");
```

It is always queued to the adoption point, and it is safe from any thread. It replaces the
public use of these, which become internal:

- `Builds::Request`, `RequestAll`, `RequestFor` and `Change`;
- `GraphResources::RequestStructuralRebuild`;
- `FrameAnchors::Refresh` and its internal `Rebuild`/`StructuralRebuild`;
- the rebuild in `PublishedResources::Store`.

`RequestAtAdoption` stays for diagnostics only.

### B12. `FrameClock` (A10)

`{beginFrame, slot, presentSerial, gpuCompleted}` is owned by `RenderGraphRuntime` and
advanced before `OnBegin`, so Begin reads the frame it starts. Every reader uses it:

- the frame driver and the revision;
- `BufferRefs`;
- DCLF, which also keeps a separately named counter for its own scene steps.

### B13. Variable table and adopted reads (A7)

- One constexpr table of `Var<T>`.
- Generic `Vars::Set(Var<T>, v, writer)`, `Vars::Adopted(Var<T>)` and
  `Vars::OnAdopted(var, cb)`.
- `CS::Setting<T>::Adopted()` as an atomic copy any thread can read.

These replace:

- the 14 typed getters and the hand-written setters;
- DCLF's `AtAdoption`, `inputsMoved`, `interiorAdopted`, `adoptedInterior` and `togglesSeen`;
- Upscaling's ready-at-once producer;
- the per-feature `frameEnabled`/`Adopted*` copies.

### B14. `CS::GpuBlock<Gpu>` (A7)

A constexpr map from `Settings` fields to GPU struct fields. It reads adopted values, and
has a colour-space hook for fields converted from sRGB to the working space. It goes in
`src/Utils`. It replaces the field-by-field copies in EHF, Dynamic Cubemaps, Terrain Blending,
ViewAtmosphere, Screen Space Shadows and the bend settings.

### B15. One image registry and one export-to-D3D11 path (A1, A12)

- `ImageRegistry::Register(image, {resource, slot, views, owner, name})`, with `Find` and
  `Retire`, and a duplicate-id check that is fatal. It replaces:
  - DxvkImageImport's `byImage`, the made-image registry and `ImageResources`;
  - FrameAnchors' `imports`/`loose`/`FindImage`;
  - native `GraphTextureOf`/`GraphTextureOfImage`;
  - `Targets::FindImage`.
- `GraphResources::ExportForEngine(desc, name)`, with one way to retire. It replaces
  EngineTargets' `RetireToken`/GUID/`pinned`, EngineTextures' `AttachGraphResource`/`Abandon`,
  the DynamicRing tokens, and `ReleaseAfterD3D11`.
- `GraphResources::D3D11Wrapper<T>`, made by `MakeWrapper(declared)`. `Revision::AdoptAll`
  re-points every wrapper at the adoption point. It replaces:
  - the hand-made wrappers in nine places (Cloud Shadows, HDR, Physical Sky, Volumetric
    Clouds ×3, Skylighting ×2, cloud maps ×2);
  - `PhysSkyFollowTwin`, `CloudFollowTwin` and its `byD3D11` map;
  - HDR's `NewExport` pointer compare.
- Longer term, graph mode stops exporting to D3D11 at all, once DCLF binds by id.

### B16. `RenderGraph::Bindless` (A12)

`Bindless::Resolve(view, tag)` and `Bindless::Null()`, owned by CS, with DCLF as one more
user. Today five non-DCLF features reach `DCLF::GpuTextures` for this: ImageSpace, Scaleform,
cloud maps, Physical Sky and Volumetric Clouds.

### B17. `Engine::UiTargets` (A1, A13)

`UiTargets::Current() -> {backBuffer, hdrUi, uiTarget, formats}` is a variable that HDR
raises. It replaces the back-buffer, HDR UI and saved-RTV logic in Scaleform, Bink, ImGui and
ORGHDRDisplay, each of which undoes HDR's RTV swap itself. It also removes the four `"hdr.ui"`
literals.

### B18. Uploads (A3)

- ImGui uses `FrameStaging` instead of its own `StagingPool` (`ImGuiBasicRHI.cpp:155-235`).
  It already includes that header.
- The EngineConstants chunk pool and DynamicRing become one `UploadRing`.
- The engine's constant-buffer pools move onto it (A3, step 8).

### B19. Engine-draw family client (`Family::Client<Run>`)

Sky, Water, Precipitation, ShadowMask, BloodSplatter, DecalOcclusion and MenuScenes share:

- the same State struct;
- the same API: `Reserve`, `Available`, `Setup`, `CanTake`, `Submit`, `Program`, `Keep` and
  `Status`;
- the same `atomic_store(run)` + `pending` + `RunWithheld` sequence;
- a hand-written FNV hash (7 copies).

Three register through `DeclareClient`. Water and Precipitation register raw.

The proposal is `Family::Client<Run>{id, anchor, makeDesc}`, always on `DeclareClient`, with
`Run::Hash()` built on a shared `Util::Fnv1a`. The FNV constant appears in 25 files.

### B20. Pass authoring (A4)

- `AnchorPasses` becomes the default way to write a pass. Hand-written `TypedRenderGraphPass`
  classes are kept only for what it cannot express. Today there are 38, with 35 hand-written
  `Revision()` pointer hashes. Their `Revision` is derived from the captured spec.
- The post `Builder`, the cloud-map `StepDesc` and FX `FxLower` are reviewed for whether they
  can produce `AnchorPasses` descriptions.
- Clears become one `AnchorPasses` clear that handles versioned slots. That covers
  `UavClearPass` (8 users), `AnchorPasses::ClearOf` (2) and ImageSpace's `ClearFirstVersion`.
  Most clears disappear into `Birth` (B5).

### B21. ImageSpace step groups

Four step groups each have a Key, `KeyOf`, a lag check, a description cache, `Build*`,
`Declare*`, row bitmasks and `requested[]` (`WorldSteps.inl:484`, `ChainSteps.inl:490`,
`StartSteps.inl:90`, `MenuSteps.inl:60`).

They become rows in `RecipeTable.h`, with predicates on description fields. If that does not
fit, they share one `StepGroup<Key>` template.

### B22. Small shared helpers

- `ShaderBindings::Sampler(Preset)`: a shared sampler cache. There are 26 `CreateSampler`
  calls in 20 files, mostly point with border and linear with clamp or wrap.
- `DxvkExports`: one table. DXVK exports are resolved twice today
  (`Upscaling/DXVKInterop.cpp:457`, `RenderGraphRuntime.cpp:2183`).
- `Util::Fnv1a` (B19).

### B23. Test harness (A14)

- An `org:` command set in `RenderGraph/Testing`, compiled out of Release.
- cs-test gains `await = "adopted"`, per-client `graph_frames` counters in the exit report,
  and a `call` step over a feature command registry.
- Saved settings can be injected before launch. That replaces `ssgi-preset` and
  `CS_LLF_LOCAL_SHADOWS`.

## C. Delete

These need no design, only care, and can land as soon as the code they depend on allows.

**Dead code**

- `FrameAnchors::ScheduledThisFrame`: no callers.
- Flush: `Reason`, `EngineDraws`, `VouchPass`, the `RenderPassImmediately` detour and the
  marks. Nothing outside `Flush.cpp` calls them.
- `ORGPostProcessing.cpp:1170`, the "has no ORG implementation" branch. All 14 effects are
  ported.
- The native resize to a new size (`PendGraphBackBuffer`). The fatal `Create` always follows
  it, so no frame uses the result.
- `targetsGeneration` and `shadowFormat` in `RevisionInputs` and `SameRevisionInputs`. They
  are dead since 10a-0. The generation-checked catalog paths in `DrawPipelines` (after A1).
- The `FrameStaging.h` include in `OcclusionQueries.cpp`.

**Fallbacks and switches (with A3)**

- IBL `onFailed`/`CreateSHTextures`, Terrain Blending `onFailed`,
  `ORGUpscaling::ReleaseAfterFailure` with `Upscaling.cpp:733`, HDR `Fail`, and Screen Space
  Shadows' mid-frame `Stop`. They are unreachable without a tap error.
- `CS_DCLF_ASYNC_BUILDS=0` and its `ownerBuilt` path (with A5, step 2).
- `Toggles::UseRevisions(false)` and the direct `ApplyAdopted` in `Commit`.
- The `TEMP (CS_DCLF_SET_PARITY)` records and mirrors (17 markers).
- `Vars::RaiseFeatureSettings`, `FeatureSettingsEvents` and the `FeatureSettings.*`
  variables, after the three remaining `Setting<T>` ports.
- `CS_ORG_LOADING_THREAD` and its loading-thread adoption path (D-4).
- `ORGLocalShadows` is not deleted. It is reworked (D-1, A13).

**Test keys and stale text**

- Test keys: the 17 `org.<feature>` keys in `tools/cs-test/schema.toml` and the
  `CS_ORG_ENGINE_FLUSH_LATCH`, `_KEEP` and `_UNBIND` switches.
- Every "`CS_ORG_<X>=1` (default off)" mention in port and engine headers and logs. Every
  `Requested()` is really `GraphMode()`.
- The `CS_ORG_TRIPWIRE_GUEST` error text in `Flush.cpp`.
- ImageSpace's "set CS_ORG_ENGINE_CONSTANTS=1".
- `Withdraw`'s "readers return to D3D11".
- `ChainSteps.inl:1` "for native mode".
- BuildManager.h's dangling "Which finished builds a poll attaches."
- The repeated doc comment at `PublishedResources.cpp:146-147`.
- `PublishedResources.h`'s "Import and publish again on a resize".
- `docs/development/dclf-architecture.md:108,177`, which still describes
  `AdoptCurrentSnapshot` and the frame-start wait.
- The shader-reload failure text: "main resources generation" and "targets generation".

**Renames and moves**

- `Wish`/`BufferWish` → the declaration names, in `DeclareLane.h` and `Declare.cpp`.
- `FrameAnchors::CommitReadRevision` → `CommitReadTicket`, to avoid clashing with frame
  revisions.
- `RenderGraphRuntime::DclfBuildsSettled` → `BuildsSettled`.
- `LoadingScreen` holds the `Renderer::Begin` hook that drives `Frame::Driver::OnBegin`. Move
  the hook to Frame.
- `IndirectDraws::AfterSceneStep` does three jobs. Split it, and drop the name once A5 lands.
- SSS reads `CS_ORG_SSGI_DEBUG`. Give it a switch of its own.
- `FrameAnchors::Refresh` has one user (the test harness), and `InBuiltGraph` has one user
  (Terrain Blending). Fold or delete them.
- `D3D12SelfTest` (504 lines, started from `XSEPlugin.cpp:136`) moves to the tests.

## D. Order of work

The phases follow dependencies. Inside a phase, the lanes touch separate files and can run in
parallel, under the current workflow: leads write code and list tests, and the director
merges, builds and tests.

**Phase 0: deletions and messages (S each, no behaviour change).**

- Section C items that need nothing first.
- A11's error messages.
- The interim `SWP_NOSIZE` drag fix.
- Lanes: core text, engine text, ports text, DCLF dead code. Testing: build plus the standard
  smoke set.

**Phase 1: tests and policy first.**

- B23: `await = "adopted"` and `graph_frames` counters. Every later phase uses these to show
  that it did not add D3D11 frames.
- B7: `PortFatal`, `ReasonLog` and the port skeleton.
- A3 steps 1, 2 and 9: setup and runtime failures fatal, recovery paths deleted, tap errors
  fatal in development builds, and the release-build DCLF menu warning (D-5).
- A6: retire `CS_ORG_LOADING_THREAD` (D-4).
- A5: lock the DCLF on/off control in graph mode (D-3, to confirm).
- B13 and A7: the variable table, `OnAdopted`, `Setting::Adopted`, and the three
  `Setting<T>` ports.

**Phase 2: core plumbing.**

- B12 `FrameClock`.
- B11 `Builds::Ask`, plus A6's thread checks and the adoption-point queue.
- B9: the executor, moved out of DCLF.
- B8 `ProgramService`, which removes the render-thread compiles.
- A5 step 1: `BuildState` carried by builds.

**Phase 3: CS owns the engine targets (B1, B4, B15, B17).** This is the largest single item.

- It starts with the guest retarget design and with native wraps on the lane (Q2).
- The present-copy investigation (A1, D-2) runs alongside. It is measurement and reading
  only, so it can start in any phase.
- When it lands, delete the `Observed` sites, `Invalidate`, and DCLF's format observation.
- Window resize then works in both modes. Test with `native-vulkan+resize`, the guest resize
  config, and a window-drag test.

**Phase 4: shared frame helpers in the ports (B3, B5, B6, B14, B19, B20, B21).**

- These are mechanical once phase 3 has removed the observed inputs.
- Spread across six port lanes, as in the feature-port phase.
- A9's queries collapse here.

**Phase 5: DCLF as an ordinary client.**

- A5 steps 2-5: plain staged epochs, the fragment source, one recording service and
  `StarvationWatch`.
- A4: shader reload as a revision input.
- A3 step 6: coverage in the recipe.

**Phase 6: no D3D11 frames left.**

- A3 steps 3-8: stages for every frame kind, holds as declared choices, engine constant-buffer
  pools as ORG buffers, and residue gated.
- The census becomes a tap error.
- A13's separate cases: LLF culling as a stage client, the presenter contract, and the D3D12
  capability.

- The local-shadow rework (A13, D-1) fits here, after phase 5 makes DCLF's shadow views
  ordinary declared clients.

Measurements that ride along without code changes: the memory items in A13, and the present
copy (D-2).

## Decisions

The user answered these on 2026-11-20. They are also recorded as item 68 in
`director-decisions-2026-11.md`.

- **D-1. Light Limit Fix local shadows:** rework the cache to follow DCLF's shadow views. Do
  not delete it. See A13.
- **D-2. Guest back buffer as a CS slot:** the cost of the present copy is acceptable. Record
  it as an area to investigate: why the copy is needed, and whether it can be removed. See A1.
- **D-3. DCLF in graph mode:** graph mode can require DCLF. Running without DCLF, or turning
  it off mid-session, is not a goal. The menu lock is the director's call, to confirm.
- **D-4. `CS_ORG_LOADING_THREAD`:** retire it.
- **D-5. Rule violations:** fatal in development builds only. In release builds, a detected
  violation shows a warning in the DCLF menu with instructions to send the logs to the
  developers. See A3, step 9.

## Handoff

This section is for a new director session picking up the plan.

### State on 2026-11-20

- **Branch:** `integ/hard-ports` in `C:\Users\matth\source\repos\SARP\external_refs\cs-org-dxvk`.
  The frame revision plan (steps 1-10 and late changes) is merged and tested on it. Nothing is
  pushed.
- **Plans before this one:** the native framegen plan and the frame revision plan
  (`frame-revision-plan.md`) are finished. Their open items are in
  `director-decisions-2026-11.md` (items 1-67). The ones this plan absorbs are tagged
  **[known]** above.
- **Open items this plan does not cover**:
  - Effects11 cannot be tested here: there is no preset on this machine.
  - DLSS-G needs an NVIDIA GPU; this machine has an RX 6600.
  - The glints refill after a shader reload logged only once (item 67).
- **Submodules:** their commits are local only, on branches inside each submodule:
  - ORG (`extern/OpenRenderGraph`) at `bfd13bb`, branch `rev-step4`;
  - OMS (`extern/ORGModuleServices`) at `d48c924`, branch `rev-step5`;
  - `extern/BasicRHI` on `org-native/fg-dlssg-native`;
  - `extern/Streamline` on `org-native/fg-native-d3d12`;
  - `extern/dxvk` on `org-port/view-trace`.

  Before any push, push those submodule branches first. Restore any submodule URL that points
  at the local main checkout (`git config -f .gitmodules --get-regexp url`).
- **Worktrees:** the port worktrees under `..\org-port-worktrees` were removed on 2026-11-20.
  Three folders stay on disk only for reference: `org-frame-slots`, `org-rebuild-race` (their
  git data was lost) and `org-p0-pipelines` (its HEAD `c5bedf3` is not in the main repos).
  Leave them alone unless the user says otherwise.

### How the director works

These are the user's standing orders:

- The director runs one lead subagent per substantial part of the plan, up to six at once.
- **Leads write code only and list their tests.** The director merges their branches into
  `integ/hard-ports`, builds, stages, tests and fixes. This avoids parallel builds of about
  7 GB each.
  - Brief: [org-cleanup/director/lead-brief-codeonly.md](org-cleanup/director/lead-brief-codeonly.md).
    Add the part's scope, a worktree under `..\org-port-worktrees`, and a branch name
    (`org-native/cleanup-<part>`).
  - [lead-brief-building.md](org-cleanup/director/lead-brief-building.md) is the older brief,
    for leads that build and test themselves.
- **Uncertain decisions go into `director-decisions-2026-11.md`.** Continue numbering from
  item 68. Leads write theirs into this plan as "decided by the agent, to confirm".
- **Only run the tests a change needs**, not the whole suite (user, 2026-11-20).
- **Errors are fatal.** No waits, retries or fallbacks. The render thread never waits. Builds
  are async. Changes publish at frame boundaries. Recordings must be replayable.
- **Never resume an agent** that has been idle for an hour or more, or whose context is more
  than half full. Hand off to a new agent with a summary.
- **Cron check-ins** arrive about every 40 minutes. Reply in one line if nothing changed.

### Build, stage, test

All of this runs from the main checkout.

- **Build:** run [org-cleanup/director/build-devfast.cmd](org-cleanup/director/build-devfast.cmd)
  with `cmd /c`. It runs the Dev-Fast preset with `-k 0`.
  - Never link while a test game runs.
  - On LNK1140 or LNK1201, delete the build's `.pdb` and `.ilk` and link again.
- **Stage:**

  ```
  python tools/cs-test/cs_test.py stage . --name nfg --preset Dev-Fast --base-aio "C:/Modding/MO2/mods/CS AB nfg"
  ```

- **Run:**

  ```
  CS_TEST_BACKGROUND=1 DXVK_DEBUG=hang python tools/cs-test/cs_test.py run <cfg> --stage nfg --wait 3600 --label <label>
  ```

  - Configs compose, e.g. `native-vulkan+resize` or `guest+dclf-toggles`.
  - Results land in `C:\Modding\CS-AB\runs\<run>`.
  - Read the exit report's tap-error and late-change counts and the DCLF wait count.
- **Post-chain tests** (`post-effects`, `post-lens`, `post-programs`) need Post Processing
  mode. Use [org-cleanup/director/ppbatch.sh](org-cleanup/director/ppbatch.sh):
  1. Back up `SettingsUser.json` to `$PP_BACKUP` (default `~/SettingsUser.backup.json`).
  2. The script sets the mode, runs the batch, and restores the backup on exit.
- **Merges:** [keepboth.py](org-cleanup/director/keepboth.py) resolves conflicts where both
  sides add lines. It keeps both sides and closes a struct that was split at a shared brace.
  A function split across a conflict needs a hand fix. Read every result before building.
- **Standard set** (pick what a change touches):
  - mode smoke: `guest-quick`, `native-quick`, `native-d3d12-quick`, `menu`;
  - `latch-census-short`;
  - `resize` in guest and native Vulkan;
  - `guest+fg`, `guest+dclf-toggles`, `native-vulkan+shader-reload`;
  - a feature's own config, e.g. `ibl`, `terrain-shadows`, `dynamic-cubemaps`,
    `screenshot-clean`, `hdr`, `upscalers`, `river`, `rain`, `flicker-taa`, `native-census`
    or `native-tour`.
- **Memory notes** (in the user's Claude memory, not the repo) cover the traps:
  - shader cache warm-up after shader changes: one run per mode with `run.timeout_s=900`, and
    never raise a timeout otherwise;
  - closing MO2 before runs;
  - the AMD equality-return miscompile;
  - eye-adaptation variance between shots.

### Where to start

1. Phase 0 (section D) needs no design. Split section C and A11 into about four code-only
   leads: core text, engine text, port text, DCLF dead code. Merge, build, and run the mode
   smoke set.
2. In parallel, start the phase 1 test support (B23). Every later phase uses
   `graph_frames` to prove it did not add D3D11 frames.
3. Then follow phases 1-6 in order. Phase 3 (CS owns the engine targets) needs a short design
   step first. That step covers the guest handle retarget and native wraps on the lane, and
   should be written into A1 before any lead starts.
