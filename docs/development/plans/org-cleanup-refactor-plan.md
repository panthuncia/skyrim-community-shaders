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

**Findings (phase 3, 2026-11-20).** The reasoning is in
[phase3-engine-targets-design.md](org-cleanup/phase3-engine-targets-design.md), section 7.

- **Why the copy is needed.**
  - Guest: DXVK already copies once. `D3D11SwapChain::PresentImage` blits DXVK's back buffer into the Vulkan swap chain
    image (colour space, HUD, scaling), in DXVK's own stream, so neither CS nor the engine can name that image. DXVK's
    back buffers also rotate storage at every present. With kFRAMEBUFFER as a CS slot, CS adds one copy: the slot's
    current version into DXVK's back buffer, at Present (`DxvkPresenter::CopyFrame`).
  - Native Vulkan and D3D12: the swap chain image (DXGI's buffer index) is known only after the previous present,
    which runs on the submission thread while the render thread records the next frame. Recordings replay across
    frames and must name one back-buffer version. So the copy (`PresentThroughLedger`) is the late-bound step,
    recorded at present. Formats match by construction (no sRGB format on the swap chain).
  - Frame generation (DLSS-G, FSR-FG in guest; the presentation layer in native) takes the presented image itself.
    It neither needs CS's copy nor stops CS writing its images. The blocker is the recording, not frame generation.
- **Can the last write target the acquired image?**
  - Guest: CS's copy can go, DXVK's blit cannot. A small DXVK interop export (`dxvkSetPresentSource`, interop
    version 8) would make the next `PresentImage` blit from the slot's current version instead of `m_backBuffers[0]`.
    Nothing in any recording changes.
  - Native: only for a single last pass recorded every frame, against the acquired image. With HDR on, HDR's output
    composite could be that pass (a raster pass; storage usage on swap chain images depends on the surface). In SDR the
    back buffer has several writers (post chain, Scaleform, Bink, ImGui), so the copy stays.
- **Measured (RX 6600, 1080p, `perf-taa`, 900 frames between the marks, one run each; `CS_GPU_EVENT_TIMERS=600`).**

  | Mode | Copy on the GPU | fps with the copy | fps with `CS_ORG_PRESENT_COPY_SKIP=1` |
  |---|---|---|---|
  | Guest (`CS present copy`) | 0.03 ms | 41.3 | 41.6 |
  | Native Vulkan (`native present copy`) | 0.03-0.04 ms | 41.2 | 41.8 |

  The copy costs about 0.1 percent of a 24 ms GPU frame. The fps differences are within the run-to-run spread of
  these samples (single 5 s readings range over about 1.5 fps). 4K was not measured (it needs SkyrimPrefs edits);
  scaled by bytes it would be about 0.12-0.16 ms.
- **Conclusion.** Keep the copy in every mode. It is far below the 0.2 ms the design set as the bar for native's
  HDR-only late pass. The guest present-source export stays a possible follow-up, not worth doing for 0.03 ms.

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

Step 9, decided by the agent, to confirm (lead-core, phase 0):

- In release builds the affected work is still skipped, as before. The caller draws nothing.
- The boundary census now runs in release builds too. It takes one lock per adoption.
- There is no separate DCLF log file. DCLF writes to `CommunityShaders.log`, so the warning
  names that file and shows the folder from `logger::log_directory()`.
- Failed anchor clients, recipe mismatches and world recipe order findings raise the warning
  too.
- `CS_ORG_MISMATCH_FATAL=0` raises the warning in development builds.
  `CS_ORG_TAP_ERROR_TEST=<frame>` raises one tap error for tests (config `tap-warning`).

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

Decided by the agent, to confirm (lead-core, phase 0):

- `DeclareTexture*` rejects every Transient lifetime, exported ones too. An export ignores the
  lifetime, so those were Persistent already. The callers now declare Persistent.
- Effects11's row table keeps Transient to mean "needs no clear", and declares those rows
  Persistent.
- `WhyNotPreparable`'s last cause still joins "not a texture" and "changes its shape class",
  since `ShapeAt` returns one bool.

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
  - **Map (LLF lead, phase 6).** How DCLF names a local light's shadow views:
    - Selection: `LightSelection` (`Features/LocalLights/LightSelection.cpp`) detours
      CalculateActiveShadowCasterLights. It lets the engine select, then selects again with its own
      port: `Visible` ports vfunc 0x10 (UpdateCamera) inline, and the room test uses its own portal
      walk. It keeps at most four lights, the sun first, and publishes the slots
      (`ShadowViews::LightSlots()`) for its scene frame. LLF's rotation hooked vfunc 0x10 through the
      vtable, which this port never calls: that was the conflict.
    - Cameras: `LightViews` (`Features/Shadows/Local/LightViews.cpp`) computes each kept light's
      camera per descriptor into a board; the Spot and Point sources hand them to the shadow engine.
    - Views: `ShadowViews` (`Core/Handoff/ShadowViews.h`) lists one view per shadowmap descriptor, in
      the engine's order, rebuilt at `ListShadowViews` when CalculateAndDrawShadowCasterLights
      returns. A view has its light, kind (Frustum is a spot light, Parabolic a point light with both
      hemispheres in one slice), descriptor, `focus`, `covered` (DCLF draws it this frame) and the
      predicted `drawTarget`/`drawSlice`. For a local light these are the descriptor's
      `renderTarget` (kSHADOWMAPS) and `shadowmapIndex`.
    - Drawing: `IndirectDraws::CaptureShadowView` (`ShadowCapture.cpp`) captures each covered view
      (target, slice, viewport, the engine's view-projection). `ShadowEpochs.cpp` and
      `ShadowEpochPasses.cpp` draw all of them in the one ShadowView epoch, into the engine's imported
      shadow map arrays (`ShadowResources::depth`).
    - What the cache needs per light: the slice of descriptor 0's view in kSHADOWMAPS; the
      projection (descriptor 0's `lightTransform`) and `shadowBiasScale`, which the engine still
      writes, since DCLF draws with the engine's camera; the type (spot, or parabolic with
      `shadowMapCount`). Update rate: every frame DCLF draws the light. There is no rotation, so a
      light DCLF does not draw keeps its cached slot until it is evicted.
  - **Done (LLF lead, phase 6).** The cache reads DCLF's drawn slices. DCLF exposes
    `DCLF::Integrations::LightLimitFix::DrawnLocalShadowViews` (`LocalShadowViews.h`): this frame's
    covered, non-focus descriptor-0 views of spot and point lights, once per light, with target and
    slice. In graph mode LLF installs no rotation hooks; at EarlyPrepass it tracks its casters, takes
    the views (`FollowDCLFShadowViews`) and copies those slices at the EarlyPrepass anchor as before.
    The block is gone; EHF's input works again unchanged. `ORGLocalShadows::Setup` failures are fatal.
  - **LLF culling done (LLF lead, phase 6).** `cs.llf.light-culling` is a declared client in the
    pre-main stage's Prepass step, ordered first there. `Segment::LightCulling`, the foreign step and
    DCLF's foreign epoch entry are gone.
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

Phase 1 (lead-vars) decided these, by the agent, to confirm:

- `Setting<T>::Adopted()` reads a registry-owned cell: one atomic word up to 8 bytes, a lock above. Before revisions
  start, or unbound, it reads the live value. Settings above 32 bytes have no adopted copy (a static assert).
- `Vars::OnAdopted` listeners get the value before and now, and run only when the adopted value changes, at every
  adoption including the first. They run inside `Lane::Adopt`, after the settings' copies are published.
- `UpscalerOptions`' accessor is `Vars::UpscalerOptionsVar()`, because the struct has the plain name.
- The DclfToggles variable is gone. DCLF's requested set is the setting `DCLF.Toggles` (the packed word, not
  normalised). `Toggles::active` stays as the adopted word normalised, written only by the setting's listener, so
  the hot readers normalise nothing. `frame` and the thread-local snapshot stay: they follow the installed scene,
  not the revision.
- DCLF's graph conditions are made again at every `UpdateGraphConditions` call from the adopted values and compared,
  instead of on a moved-inputs flag. The pass cuts' text is taken at the adoption of the DclfPassCuts event.
- Upscaling's colour-space-only switch: the producer returns no wait and remembers the target. A `PresentationConfig`
  listener asks the presenter at the adoption. A producer may now return a wait with no poll, meaning no work.
- D-4: revisions, `IssueAdoptionRequests`, the missed-writer check and the engine sampler run on the main thread only.
  A loading screen's or the startup movie's thread still owns rendering, and its frames run the revision the main
  thread adopted last.
- PostProcessing has no `Setting<T>` port. Its projected fields are strings or per-frame constants, and its ORG chain
  already keys on what shapes it (the tonemap define hash, the LUT's identity).

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

Decided by the agent, to confirm (lead for B23, 2026-11-20):

- `await = "adopted"` compares the frame variables' change serial: it reads the newest one a Begin took two Presents
  after the previous step, and holds until the adopted revision carries it (`Revision::Mark`). Build-only commands change
  no variable, so they hold at once. Why: every write that shapes a frame goes through a variable; two frames cover the
  main thread's follow-up writes.
- A port's D3D11 frame counts only after the client's first graph frame (`d3d11`); earlier ones are `d3d11_before_graph`.
  Why: the clients start up on D3D11 for a few frames, which `effects11` already allowed.
- `expect.graph_frames` is skipped when `CS_ORG=0`, so `d3d11+<feature>` compositions still pass.
- Fallbacks are counted at three sites only (Effects11's rays and FX chain, Exponential Height Fog). The "returns to D3D11"
  paths are left to A3, which makes them fatal.
- `org:refresh=<id>` asks `RequestAtAdoption(id)`: the graphs that looked that graph id up. A client id is not a graph id;
  `FrameAnchors::Refresh` has no user left and can go (lead-core).
- `ssgi-preset` became a `setting` step with an object (`ScreenSpaceGI={...}`), and the run starts from the standard preset
  in `[saved_settings]`. Why: a preset is several settings written at once, which any feature can use.
- The schema generator drops every scenario-only `CS_ORG_*` switch that no source reads (47 keys with
  `cs.llf_local_shadows`), not only the 17 named ones. The flush-latch `_KEEP` and `_UNBIND` keys go once their code goes.
- The render graph's harness namespace is `RenderGraph::TestSupport`, not `RenderGraph::Testing`, which would shadow
  `::Testing` inside `RenderGraph` in unity builds.

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

Phase 0 core lane, decided by the agent, to confirm (lead-core):

- The flush latch itself stays. It is on in every graph mode (`LatchRequested()` is
  `GraphMode()`). Only `_KEEP` and `_UNBIND` were real switches. `CS_ORG_ENGINE_FLUSH_LATCH` is
  left only in `schema.toml`.
- Without `PendGraphBackBuffer`, a native resize at the same format keeps the old graph present
  slot until the fatal target set. The format-change branch is unchanged.
- The interim drag fix is in `Hooks.cpp` only. `EngineTargets::BeginSet`'s fatal message is
  unchanged.

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

## Open questions

### DCLF lead, phase 0 (decided by the agent, to confirm)

- **Set parity switch kept.** Only the 17 TEMP records and their mirrors went (with the shader's `RecordWriter`).
  `CS_DCLF_SET_PARITY` and its other checks stay, since they have other uses (director agreed, to confirm).
- **Catalog checks without the revision inputs' copies.** With `targetsGeneration` and `shadowFormat` gone from
  `RevisionInputs`, `GetIndirectState`, `CatalogFamilyPipelines` and `GetShadowIndirectState` are checked against the
  catalog's own generation and format. A target or format change after DCLF draws is a tap error, and a new catalog makes
  a new revision. The generation checks themselves go with A1.
- **Toggle commits before the frame driver are fatal.** The driver installs before DCLF in every mode, so this cannot
  happen today.
- **Staged refusal made fatal on the DCLF side.** `IndirectDraws::DropScene` fails on `SceneDrop::Refused`. The comments
  in `Frame/Revision.cpp` (`AttachSceneBuilds`, the revision's scene step) still say the scene is dropped and rebuilt;
  they belong to the core lane.
- **The menu lock is on the generic boot toggle.** DCLF's on/off control is the feature header's boot toggle, so the lock
  is a `Feature::GetBootToggleLock` hook drawn in `FeatureListRenderer.cpp` (disabled toggle, short caption in place of
  "Load at Boot", tooltip). The CS editor's boot toggle and "Enable at Boot" button rely on `ToggleAtBootSetting` refusing.
- **DCLF's own fault paths are fatal in graph mode.** A failed graph epoch and a stopped scene pump switched DCLF off
  mid-session, which is the blank world D-3 rules out. They now fail in graph mode, and still hand drawing back outside it.

### Ports lead, phases 0 and 1 (decided by the agent, to confirm)

- **Skeleton is a CRTP base, and the non-graph stubs stay.** `RenderGraph::Port<T>` (`src/RenderGraph/Port.h`) gives
  `On()`, `Get()`, `IsActive()` (impl set), `ReserveAnchors` and `Fatal`. `Requested()` became `On()` at the call sites.
  ORG files still build without `CS_HAS_RENDER_GRAPH`, with a short `#else` of feature-specific stubs. Leaving them out of
  the build would need about 150 guarded call-site blocks in 55 files, so that waits for a later phase.
- **The existing `Util::ReasonLog` (`Utils/Logging.h`) is used.** No second class. It logs each reason once a session, so a
  reason that comes back is not logged again, where `fallbackReason` logged every change.
- **Post Processing's `SetReason`/`SetSkipped` and Physical Sky's AP input note stay.** They also log the return to the
  graph, and they belong to the phase 6 holds.
- **Setup runs once a session.** A second `Setup` returns without tearing down, since the outputs are published.
- **HDR Display's scene clear program is required.** It was optional, with a D3D11 movie-frame clear.
- **Physical Sky's AP passes are required.** Without their twins or anchors the AP shadow, sky tail and composite stayed on
  D3D11.
- **Cloud Shadows' layer operations are required at setup.** The per-operation D3D11 refusals in `QueueLayerOp` stay
  (phase 6).
- **A Physical Sky clouds frame with no inputs wired is fatal.**
- **Upscaling (worker):** a record that cannot be described is fatal. An SDK refusal is logged once per label.
- **Deferred clears (worker):** `CS_ORG_DEFERRED_CLEARS=0` is no longer read. Its SwitchRegistry entry can go.
- **FX runtime (worker):** no graph host or Tonemap epoch at a tonemap is fatal, where it was an FX11 frame.
- **Light culling (worker):** a failed foreign tap still returns false, which gives one D3D11 culling frame.
- **LLF local shadows:** only the skeleton changed. Its nine setup returns that log "stay on D3D11" wait for the rework (D-1).
- **Not changed:** the CS_ORG_SSGI_DEBUG value 16 is unused, kept so 17 keeps its number. `GrassCollision.h:85` and
  `LightLimitFix.cpp:650` still name removed switches, since lead-vars and the DCLF block own those lines.

### Engine lane, phase 1: decided by the agent, to confirm

- **`Engine::On()` excludes VR** for every engine module and ImGui. Before, Buffers,
  Constants, ImageSpace, Scaleform, Skeleton and ImGui ran in VR graph mode while Bink,
  OcclusionQueries and Textures did not. One rule avoids a feature split across two paths.
- **The runtime fault policy keys on `Engine::On()`**, so VR keeps today's "graph off, D3D11
  on" after a fault. In graph mode a failed `Initialize` is now fatal too. That includes guest
  mode with no DXVK loaded, which used to run the game on D3D11 with every engine module off.
- **A failed epoch is fatal in graph mode**, both the whole-graph and the single-epoch cases.
  The client drop and rebuild in `FrameAnchors::OnEpochFailed` and DCLF's
  `OnGraphEpochFailed` now run only outside graph mode. An epoch that ran past its published
  copies (`EpochCopiesBusyError`) still skips the run: it is a schedule overrun, not a failure.
- **A Skeleton call site another plugin patched is fatal.** Graph mode has no engine path for
  that function's clears.
- **ImageSpace's per-draw `Failure()` is fatal** (a missing captured constant buffer). Its
  per-draw `Refuse()` and the "ran without DrawTriShape" reports stay for phase 6.
- **Left for phase 6, as per-frame residue:** Skeleton's per-clear `Refuse` and lent-slot
  clears; Bink's off-thread, odd-tile-layout and not-yet-registered ticks; Scaleform's
  per-segment `Failure` and failed pipeline keys; EngineTextures' "left to the engine"
  textures (still usable DXVK textures); ImageSpace's build-time "pass is left out" errors.
- **A13, D3D12 and DCLF:** no new capability was needed. DCLF already reads
  `IndirectCommandsFeatureInfo` once at `DrawPipelines` setup, and D3D12 reports no pipeline
  sets, so `DeviceGenerated()` is false and every `*_PLAIN` bucket is on. The four switches
  `Native.cpp` forced were redundant and are gone, with `D3D12DclfOff`.

### A5 step 1 lead, phase 2 (decided by the agent, to confirm)

- **The build state reaches the factories through the build manager.** ORG's extension factories take no argument, so a
  build carries an opaque input (`BuildManager::SetBuildInput`, read with `Builds::Input`). The manager takes it on the render
  thread when it makes the job, beside the `Defer` context. An ORG factory argument would drop the thread-local delivery,
  but the host's own render-thread builds would still need the value at that point.
- **The host's own builds take the value when they run.** An epoch's first build, setup and the pipeline swap build inline
  on the owner thread, so `Builds::Input` asks the source there. That is the old `SetBuildStateSource` fallback, now in the
  manager. Anywhere else a factory gets null: it logs a warning and makes no passes, where it used the attached value.
- **Frame code reads `AttachedGraphConditions`.** The direct sites, coverage, `RevisionOf`, first person's begin and the
  diagnostics read the attached graphs' conditions by that name. `RevisionOf` copies the forward fields into
  `EpochRevision`, so a commit reads one set. Using the adopted snapshot's `build` there instead is a later choice.
- **Passes copy what they read.** A pass that reads conditions at preparation or recording keeps a copy of the build's
  `GraphConditions` or of the field (BuildDraws, the grass and tree LOD culls, the latched copies, pixel rows, texture
  transforms). `~latched-record` became a per-pass flag, not a global.

### Executor lead, phase 2: A5 step 3 and B9 (decided by the agent, to confirm)

- **The executor is CS code, not OMS `ServiceExecutor`.** `ServiceExecutor.h` is only a submit hook type, with no threads.
  DCLF's `SceneExecutor` moved whole to `RenderGraph/Async/Executor.{h,cpp}` as `RenderGraph::Frame::Executor`, still its own
  library without the PCH (`cmake/FrameExecutor.cmake`, which replaces `DclfSceneExecutor.cmake`). It now links whenever the
  render graph is built. `SharedExecutor()`, `MakeLane`, `Lane` and `JobLane<T>` are in `RenderGraph/Async/Lanes.h`.
- **Task classes keep their names.** `Coordinator`, `Preparation`, `Scene` (the one serial thread) and `Streaming` (the
  blocking threads). Renaming them touches about 70 DCLF call sites for no behaviour change.
- **Thread switches renamed.** `CS_DCLF_WORKERS`, `CS_DCLF_FRAME_WORKERS` and `CS_DCLF_ASYNC_PRIORITY` are now
  `CS_ORG_WORKERS`, `CS_ORG_FRAME_WORKERS` and `CS_ORG_ASYNC_PRIORITY`, read by the core. The old names are gone, with no alias.
  The threads are named "CS coordinator", "CS serial lane", "CS worker N", "CS frame worker N" and "CS blocking worker N".
- **DCLF keeps its names for the shared executor.** `DCLF::SceneExecutor` is an alias, and `SceneScheduler::Executor()`
  returns `SharedExecutor()`. `SceneScheduler::Scope()` and `Graph()` stay DCLF's. `MakeLanePump` and `SceneLaneScope` are gone.
- **`EventQueue` moved with the clock.** The retirement clock needs it, so it is `RenderGraph/Async/EventQueue.h` now. DCLF's
  `Core/Runtime/EventQueue.h` is a using-declaration, since about 80 DCLF uses name it unqualified.
- **Two threads collapsed onto the executor; ordering kept.** Both were one FIFO queue run one job at a time, and a
  `JobLane` is the same. What changes is priority. `ProgramCache`'s builds run on the blocking class at lowest priority,
  next to texture streaming's window reads and the frame's programs lane, like that lane. `BuildManager`'s builds run on the
  preparation pool at below-normal priority, like DCLF's own graph build lane. The render thread still waits on these in
  `ProgramCache::GetNow`/`Require` (B8 removes them) and on `BuildManager`'s build mutex, now behind lower-priority threads.
- **DCLF's lanes keep their refusal handlers** (logged errors, the scene pump's failed flag). The core's lanes are fatal on
  refusal. Making DCLF's fatal belongs with the DCLF error-policy work.
- **No hand-declared DCLF functions removed.** The five in `Revision.cpp` (`SceneTaken`, `InstallScene`, `DropScene`,
  `SceneRecordedFrames`, `AfterSceneStep`) all serve the scene fragment (phase 5). The core's DCLF includes for the executor
  and the clock are gone. `DrawcallLimitFix::OnGraphSubmissionsRetired` is gone: the runtime feeds the core clock directly.
- **The release guard counts every executor thread.** `CS_DCLF_RELEASE_GUARD` used DCLF's thread tags. It now asks
  `OnExecutorThread`, so releases on the core's lanes count too.

### Frame lead, phase 2: B12, B11, A6 (decided by the agent, to confirm)

- **`FrameClock` is header-only** (`RenderGraph/FrameClock.h`): atomics only `RenderGraphRuntime` writes (friend). It moves
  without a runtime too. The frame driver's `Renderer::Begin` detour calls `RenderGraphRuntime::OnRendererBegin()` after the
  render-owner claim, then `OnBegin`. `beginFrame` moves at every Begin on any thread, like the description's frame did.
- **`presentSerial` starts at 1 and moves at `SealPresented`.** `BufferRefs::RefFrame()` reads it, so latches keep their
  present boundary. Moving them to `beginFrame` would move latches made between Present and Begin to the older frame.
- **`Description::frame` and `slot` stay**, as copies of the clock. Core readers (driver, revision, declarations, anchors,
  recipe) read the clock. Port readers of `Driver::Current().frame` were left, since the value is the same.
- **DCLF's counter is its scene step.** `FrameView::GetFrame` and `Coordinator::GetFrame` became `SceneStep` across DCLF.
  The three `GetFrame() + 1` sites became `FrameView::NextSceneStep()`. They compare with scene-step values
  (`scenePassDoneFrame`, `commitFrame`), so they cannot read `beginFrame`. Making the scene step move at the CS Begin
  would change DCLF's domain, so that was left.
- **Not frame counters, kept:** `framePaceValue` (a GPU timeline value), Capture.cpp's `presentCount` (a DCLF diagnostic for
  D3D11 mode) and the static in `CommitReadTicket` (a ticket nonce, now `ticketSerial`). No other serials were renamed.
- **`Builds::Wanted` has a `change` field**, for `Change`'s one caller (`NativeDevice.cpp`, an exempt texture promoted).
- **`Ask` before the first frame, or inside the adoption point, is made at once.** Setup builds (`BuildPending`,
  `BuildSetupGraphs`) need setup requests made before the first Begin.
- **Only structural asks are late changes.** A non-structural ask inside the frame waits for the adoption point, so it is no
  longer counted. A structural ask means an identity no version covers changed inside the frame, so it is still a late change
  (`Boundary::Change`, "structural rebuild"). `NoteBuildRequest` is gone.
- **`RequestAtAdoption` stays for the diagnostic callers** (GPA, NvPerf, `org:` test commands). The anchors' added imports
  and `PublishedResources` use `Ask`.
- **A mid-frame ask lands one Begin later than before**: it is made at the next Begin and built after it, where it used to
  reach the build thread at the frame's end poll.
- **Epoch failures are polled only at the adoption point**, first in `OnBegin` on the thread that owns rendering (decision
  70). The polls in `SignalFramePace`, `FrameAnchors::Run` and DCLF's `WatchGraphFailure` went. DCLF's shader-reload wait
  keeps its poll: it runs inside the adoption point.
- **The shared constants are made in `Engine::Setup`**, after `Initialize` and after `State::SetupResources` sized
  FeatureData. Without them, a client that reads them fails to register through `FailClients` (fatal in development
  builds). `SharedConstants::Failed` is gone.
- **The render-thread check is `IsRenderOwner`.** Declarations and `Evaluate` assert it, since revisions adopt on the
  thread that owns rendering (decision 70). The boundary census counts it instead of its own last-Scope thread (the same
  thread, since the claim comes first in the Begin detour). `IsMainThread` is left for what only the main thread does: DCLF's
  scenes and the engine sampler. Risk: a declaration made on the main thread while the startup movie's or a loading
  screen's thread owns rendering is now fatal. That is the race the check is for, since the registry has no lock.
- **`Frame::Resources::Texture2D` is not converted.** A declared texture cannot be Transient (`DeclareTexture` fails on it),
  and its users are ImageSpace scratch textures that alias today (`HdrScratch` and `LdrScratch` at display size). A
  Persistent declaration would cost that memory. This needs a transient declaration kind, or B6 and phase 3.
- **`InBuiltGraph` is not folded.** Its one user is a port file (Terrain Blending, lead-ports), and it needs a check of
  whether a client's commit can run before its build attaches.
- **`Wish`/`BufferWish` became `TextureExtentFn`/`BufferElementsFn`**, the declarations' `...Fn` naming. `wished` is `asked`.
- **`Frame::Driver::Install` moved** to `Engine::InstallGameHooks`, right after `LoadingScreen::Install` (same order).
- **VL's mid-frame `DeclareVanillaVolume` is left for phase 3.** It runs on the render owner in world frames, so the new
  thread check passes.

### A5 step 2 lead, phase 5 (decided by the agent, to confirm)

- **DCLF's epochs are "lane-only" epochs** (`BuildManager::Desc::laneOnly`), a generic field. They are staged, they form their own
  group at each poll (so they stage together, as the external group did), and `Settled()` reads only them. The core still lists
  them through `IsDclfSegment` in `RenderGraphRuntime`; the fragment source (step 4) removes that.
- **The first build is on the lane, gated on a build input.** A lane-only epoch with no graph is due once `SetBuildInput` gave an
  input (DCLF's first build point). Its build attaches when it is staged (ORG attaches at once when there is no graph). Before DCLF
  registers, nothing changes: setup's `BuildSetupGraphs` still builds every graph that has none, before the first world frame.
  `BuildsSettled` is false while a lane-only epoch has no graph, so DCLF makes no snapshot until its first graphs are attached,
  and the engine draws those frames (`NotDrawingYet`). The first adoption at setup's build point (A3 step 3) needs setup to
  happen after DCLF registers; that is left to A3.
- **The boundary hook is a generic poll listener** (`BuildManager::SetPollListener`), called at the same point in `Poll` with the
  epochs staged or attached there. DCLF settles and posts its revision inputs when one of them is its own. Ordering is the same,
  except that first builds attached at once now also count as "staged" (the owner-built path counted them too).
- **When DCLF had no graph, the first settled state is published as attached** (as before, when the first submission built
  from the newest state). A first build handed out with an older state than the one settled would be mislabelled; the old path
  assumed the same.
- **The submission build stays a warning.** `ExecuteEpoch` still builds an epoch with no graph and logs it. Making it fatal for
  lane-only epochs waits for a test run that shows it never happens.
- **`BuildsSettled` was renamed here** (it was still `DclfBuildsSettled` on integ/hard-ports).

### Lane T lead, phase 3: tripwire retarget, lane wraps, native present (decided by the agent, to confirm)

- **Retarget keeps the API of design 1.3.** `HandleCore` holds `wrapped` and an immutable `HandleState` (graph, owner) behind
  atomics; resource and view handles keep two descriptions. The old object and state go through `ServiceRetired`. The adoption
  check is `Frame::Boundary::InAdoptionPoint()`; the self-test is let through without opening a Scope.
- **Dirty bits are a tripwire helper.** `Tripwire::MarkEngineBindingsDirty()` writes design 1.5's bits, so lane A calls it once
  after its retargets instead of touching the shadow state itself.
- **Unbacked drops live in the guest context, not in the census filter.** The guest context's clears, copies and GenerateMips
  check `IsUnbacked` on their handle arguments. Clears are dropped silently (as `dropClears`), copies and GenerateMips go to
  `Frame::Driver::NoteUnbackedUse` (fatal in development builds). Draws that bind an unbacked handle get null from `Unwrap`, as
  before. `Tripwire::UnbackedDrops()` counts them for the census line.
- **Wrap records are Host, not Tracked (design 2.2 item 3).** Native wraps stay `Ownership::Host`. Their context work still
  follows the ledger: `LedgerResourceOf` finds the export through the made-image registry when it is graph-managed. So the
  deferred initialize needs no ownership change.
- **First contents of a native exported version (design 2.2 item 4).** OMS/ORG have no "clear on first use"; OMS's
  `TextureContents::Zero` zeroes on the uploader's copy queue, for concurrent colour textures only. So a native colour version is
  made `QueueSharing::Concurrent` and born `Contents::Zero`; the uploader leaves it in Common and the revision is ready only once
  the fill is done. A depth or multisampled version cannot be filled there: it is born `Undefined` (tracker seeded `Undefined`),
  so its first writer discards, and the engine clears its depth targets before reading them. No OMS branch was needed. Risk to
  check on the RX 6600: a concurrent image may lose DCC, which costs on large colour targets.
- **Guest wraps keep their image.** A guest version's DXVK wrap is made on the lane and carries its graph resource
  (`AttachGraphResource`), so a revision dropped before adoption cannot free an image DXVK's zero fill still writes.
- **Native back buffer slot (design 3.7).** `Native::UseBackBufferSlot(slot, field)` is the seam lane A calls from `DeclareSet`.
  Until then native keeps its own slot, so native present works unchanged. After it, the present copies the slot's current
  version through the ledger (a refusal is fatal), `GetBuffer` answers the field, and a swap chain resize only resizes the swap
  chain (a format change is fatal). `MakeBackBuffer` and its relatives stay until lane A lands, then go.
- **`DisplaySize` is still written by native `Resize`.** Design 3.3 moves it to the set's `EndSet`; lane A cannot edit
  `NativeDevice.cpp`, so the director removes that line when lane A's `EndSet` writes it.
- **Native `Registry::byImage` stays (design 4.2).** Both callers of `GraphTextureOfImage` are lane A's files, and registering
  native tracked textures in the made-image registry would change how FrameAnchors imports them. Lane A's `ImageRegistry`
  replaces it: `NoteOwnedImage` calls `ImageRegistry::Register`, `~NativeResource` calls `Retire`, `GraphTextureOfImage` goes.
- **The present copy timer is a sub-range.** "native present copy" is `GpuTimingSource::AddExternalRange` with
  `RangeKind::Sub`, so it adds a row without changing any other range's self time. It is read when its present slot comes round.

### Programs lead, phase 2: B8 and A4's program items (decided by the agent, to confirm)

- **One service, three ways in** (`RenderGraph/ProgramService.h`, which replaces `ProgramCache`). One `JobLane` on the
  executor's blocking class builds every program. `DeclareProgram` posts there too, so the frame's own programs lane is gone.
  - Keyed on variables: `DeclareProgram`, as before.
  - Fixed: `Programs::Queue` at setup returns `Programs::Fixed<P>`, whose `Get` is fatal before the build is done.
  - Keyed on observed state (a target format, a draw state): `Programs::Table`. `Want` posts once, `Find` never builds.
    A client key whose program is still building calls `Frame::HoldForPrograms`: the registration stays as it is and the
    client is evaluated again at the next revision start.
  - `GetNow` and `Require` are deleted.
- **The setup batch is joined once, at the end of CS's setup** (`Engine::PostSetup`, before `BuildPending`). This is a
  render-thread wait, but at setup, where the setup graphs are already built inline and before any frame. It goes away when
  the setup builds become async. Fixed programs queued after it gate the next revision instead.
- **Every build in flight gates every revision start** (superseded: see the programs fix lead below). `ProgramsReady` returns false while any fixed or table build is in
  flight, not only the ones the revision needs. A table's program is visible to `Find` from the revision start that
  collects it, so a frame never sees a program appear mid-frame.
- **`DeclareProgram` builds nothing at declaration.** Its first key builds on the lane, and the first revision waits for it.
  `Adopted().Program()` is null until that revision is adopted.
- **The key function runs only at a revision start with a change.** It is remembered by its dependencies' serials (the
  last 8). `For()` and the adoption look keys up there and never call it. So the post chain is no longer replanned at every
  adoption.
- **Planning stays on the render thread, once per key change.** The post chain's `Effect::Programs` reads live feature
  objects (Color Grading's tonemapper), and FX's planner uses the effects' lowered-pass cache that `Make` also uses, so
  neither is safe on the lane yet. Moving them needs those reads to come from the key.
- **Scaleform:** a draw whose pipeline is still building holds its whole segment back (lagged, left out, not refused) until
  a revision start collects the pipeline. The 3D draw states are still not enumerated ahead.
- **ImageSpace:** a step's spec wants its program when it is made. A program still building drops the group's build and
  holds its clients (`BuildStepsOnce`). A spec whose effect or blend/depth states the engine has not made yet wants nothing,
  and `MakePass` leaves its pass out, as before. Before, `GetNow` built it at the graph build when the states were known by then.
- **Skeleton:** after setup, a remade target in a new format waits for its clear program. The group stays stale and is built
  again at the next adoption point, so this checks readiness once a frame while it waits.
- **AnchorPasses' depth copies:** the four depth formats' programs are queued at `Engine::Setup`, and a pass reads its program when it is made.
- **GPU load (`CS_GPU_LOAD`):** the program is queued at its lazy registration, and the pass dispatches nothing until it is built.
- **Deferred composite:** `ClearShaderCache` no longer rebuilds its programs on the render thread. Its defines depend only on the loaded features.

### Blocks lead, phase 4: B14, B19, B22 (decided by the agent, to confirm)

- **`GpuBlock` reads `Setting<T>` fields adopted and plain fields live.** Plain fields do not shape the frame, so their live
  value is the one the frame uses. Each feature's post-fill rules (EHF's suppression and volumes-not-written checks) stay after
  the fill. The colour hook is a callable passed to `Fill`, so `src/Utils` includes no feature.
- **`TerrainBlending::FrameEnabled` is always the adopted `Enabled`.** It used the live value while the ORG port was inactive.
  Without frame revisions `Adopted()` is the live value, so D3D11 mode is unchanged; in graph mode every reader now follows the
  revision, including the GPU block.
- **`Family::Client` is family-level.** One object per family holds every view's client slot (Water, ShadowMask, Sky and
  MenuScenes have several). Programs stay with the families (another lead's code). MenuScenes also registered raw, so it moved
  to `DeclareClient` with Water and Precipitation. The three moved families key on nothing (`NoKey`): their targets are imported
  once at setup, as before. Their non-fatal "could not be registered" path is gone; a refusal is fatal in `Declare.cpp`.
- **FNV.** `Util::Fnv1a` (64-bit, per-word `Mix`, per-byte `Bytes`) and `Util::Fnv1a32`. Hashes whose mixing is not plain
  FNV-1a (an extra `h ^= h >> 29`, a witness passed by reference, wyhash, FNV-1 order) keep their shape and take the shared
  constants, so values stay the same. Eleven in-memory hashes used a mistyped offset basis (`1469598103934665603`, a digit
  short); the seven families' `Run::Hash()`, `Sites` identities, `FieldsHash`, the recipe print hash and `FrameSequence`'s
  `HashText` now use the real one, so their values change. Left alone: `ProgramBuild.cpp`, `RasterProgram.cpp`,
  `ShaderPrograms.cpp` (programs lead), `FrameAnchors.cpp` (lane A).
- **Sampler presets.** `ShaderBindings::Sampler(SamplerPreset)`: one sampler per preset and device, cached for the process.
  Ports whose samplers come from engine or effect states, or with an unusual description (LocalExposure's two-axis mirror,
  Skylighting's comparison), keep `CreateSampler`. Lane A's `ORGDeferredComposite` and lane P's ImGui are not touched.
- **`DxvkExports` resolves through `GetProcAddress`, as both sites did.** `DxvkLoader`'s export-table read exists for D3D11 entry
  points that capture tools interpose; the `dxvk*` names are not matched by them. RemoteControl's GPA exports still resolve
  from the module they are passed.

### Programs fix lead: parallel builds and UI programs at setup (decided by the agent, to confirm)

Why: `native-d3d12-quick` failed at the first loading screen. The ImageSpace composites' and Bink's programs were wanted only
when the first revision built their clients (9.8 s), behind the declared programs and DCLF's manifest on the one build lane.
On D3D12 those took seconds one by one, so the loading screen (14.9 s) drew ImageSpace with no client registered.

- **The service is a pool, not one lane.** One queue, drained by several `Frame::Lane`s on the preparation class: half the
  preparation threads, so at least one is always left for the compiles the builds wait on (DCLF's build pump). With one
  preparation thread it falls back to one build at a time on a blocking thread. Jobs have no order between them.
  `Programs::Post` keeps its name. The setup batch join is unchanged.
- **Revision gating (replaces "every build in flight gates every revision start").** `Collect` returns false only for
  gating slots: fixed programs queued after setup, since any make or pass may read them. A table's build gates nothing, as
  its clients hold. Declared programs (`DeclareProgram`) still gate: the revision carries every variable's newest values, and
  nothing links a declared program to the clients that read `Adopted().Program()`, so a revision without it is not safe. With
  parallel builds that wait is the longest build, not their sum.
- **A held client is not evaluated every frame.** It is skipped until a build was collected since it held, or none is left
  (`ClientDeclaration::heldAt`, `Programs::Collected`). Without this, every revision start while builds run would rebuild the
  ImageSpace step groups and log the hold again.
- **ImageSpace's composites are wanted at the first Begin the engine has their effects** (`WantMenuPrograms`, from
  `OnFrameBegin`), not at setup: the shape log shows "effects 0 states 0" at CS's setup. Their programs' defines come from the
  engine's effect objects and their blends from its state table, which the engine makes after CS's setup. It makes the menu,
  frozen and face-tint specs once per new shape and keeps nothing. The world, chain and start steps still want theirs at the
  revision start.
- **Bink wants both targets' formats at setup:** the framebuffer's and HDR's UI texture's (another format). Both join the setup batch.
- **DCLF's engine draw programs go through the service.** `Prebuild` queues the manifest's keys at the family's setup, in
  CS's setup batch. The defines need only the shader type (new `ShaderCache::GetCompileDefines(Type, ...)`, not for
  ImageSpace), so the engine need not have loaded the family's shader. `Get` never builds or waits: a key still building or
  first seen returns null, so that draw stays the engine's (the family's existing refusal path) until it is built. An unseen key
  is posted and listed in the manifest. A failed build is now fatal at the next revision start, where before it was logged.
  The engine-draw refusal while building is a fallback the DCLF error-policy work (A3) should replace.
- **The manifest saves one at a time,** since save jobs on the pool may now run at once.

### Lane A lead, phase 3: the engine target set (decided by the agent, to confirm)

- **Handles come in two steps in guest mode.** `DeclareSet` (in `Engine::Setup`) declares every target and puts version 0's own
  objects in the fields, since the guest device does not exist yet. The state setup hook (`InstallHandles`) then puts the handles
  in and fills `Identify`'s map. Native mode does both in `DeclareSet`. A guest unbacked target gets its handle at once.
- **The retarget hook is `Revision::OnVersionsAdopted`**, a new listener list run after a revision's twins and buffers adopt and
  before its builds attach. `Vars::OnAdopted` runs before the twins, so it could not serve. The set compares the adopted
  `EngineTargetShapes` with what its handles hold and retargets only the changed targets.
- **The engine-facing views are made at adoption on the render owner,** not on the lane. A declared twin has one view of each kind;
  the per-slice DSVs, read-only DSVs, stencil SRV, cube faces and mip views are made with the device when a version is adopted.
  The whole-resource SRV/RTV/UAV are the twin's own, so `PublishedResources::IsExportOf` still matches them.
- **Mip-generating targets and their mip views stay the engine's own** (design item 7 the other way). The twin export takes no
  `MISC_GENERATE_MIPS`. At a later set the engine's function makes them again, so a port importing one would see a new texture
  outside adoption. `DeclareSet` logs which they are; mip views of a declared target (mip 0 only) are the set's.
- **Every backed target is published at `DeclareSet`,** so no reader's first ask publishes inside a frame.
- **The accumulation volumes' handles are made at the engine's first 3D create,** with its arguments (CPU only, no declaration).
  The noise volume is declared at setup, R8_UNORM at the INI size; another format from the engine is fatal.
- **VL's vanilla volume size must be known at setup;** a zero descriptor there is fatal, and a render at another size is fatal.
- **HDR keeps its LDR targets R16F in graph modes for the session,** since Upgrade/Restore return there. The session's first
  `bUse64bitsHDRRenderTarget` value is kept, so kMAIN's format never changes.
- **The size lock is off in graph modes (`Engine::On`).** VR graph mode keeps the old lock, as it has no set.
- **`Targets::Find` and `FindImage` stay as shims** over `Identify`/`Slot` and the image registry until lane D's removals merge.

### Lane P lead, phase 3: present and UI targets (decided by the agent, to confirm)

- **UiTargets is no table row.** `Engine::UiTargets::Current(inputs)` derives `{backBuffer, hdrUi, uiTarget, formats}` from
  `HdrEnabled` and the two stable slots (`Targets::FramebufferSlot`, ORGHDRDisplay's UI twin). A row would only copy HdrEnabled.
  Declarations that read it depend on `Vars::HdrEnabled()`.
- **The interface follows adopted HdrEnabled, not HDR's field swap.** Scaleform sends a display bound to kFRAMEBUFFER to HDR's UI
  surface while HDR is on; Bink's `Queue` lost its `a_hdr` argument (BinkHooks.cpp changed with it); ImGui registers for
  `uiTarget` and skips a frame whose target differs from the adopted registration's. This needs lane A to stop every
  kFRAMEBUFFER field swap in graph mode. Known change: with HDR on and the window unusable (unfocused, resizing), the overlay goes
  into the UI texture, and that frame has no composite, so the overlay is not shown until the window is usable again.
- **HDR's composite copies into kFRAMEBUFFER's slot,** not DXVK's back buffer; the present copy then moves it into the swap
  chain. A slot/output format difference is fatal. Before `DeclareSet` there is no copy pass (ApplyHDR's D3D11 copy runs).
  All four HDR D3D11 imports (`hdr.scene`, `hdr.output`, `hdr.ui`, `hdr.back-buffer`) are gone: the textures are always twins.
- **Guest present copy is `DxvkPresenter::CopyFrame(swapChain)`,** called from Hooks.cpp's `frameEnd` just before the present
  (lane A adds the call). It copies the smaller extent between a resize and the adoption, compares formats with sRGB folded, and
  `CS_ORG_PRESENT_COPY_SKIP=1` keeps the "CS present copy" event and skips the copy.
- **Screenshots of the swap chain's buffer read kFRAMEBUFFER's slot.** The capture points run before the present copy, so
  ORGScreenshot maps a source equal to `GetBuffer(0)` or the field to the slot.
- **Scaleform's `EngineTargetOf`/`EngineDepthOf` stay.** Displays carry unwrapped DXVK textures, so `Targets::Identify` (field
  handles) needs `Renderer.cpp` to keep the field index it already reads from the shadow state. Left for lane D or a follow-up.

### Lane D lead, phase 3: DCLF and engine modules on the target set (decided by the agent, to confirm)

- **Formats are read once in `DrawcallLimitFix::PostSetupResources`** (new `DCLF::DeclaredTargets`), not in `SetupResources`.
  D3D11 mode makes Deferred's G-buffer only in `Deferred::SetupResources`, which runs after every feature's `SetupResources`.
  Graph modes read `Targets::ViewFormat`; D3D11 mode, where the set does not exist, reads the engine's fields once.
- **The main pass's targets are a fixed table** in `DeclaredTargets.cpp` (kMAIN, kMOTION_VECTOR, then Deferred's six), the
  order `Deferred::StartDeferred` binds. Design 5.1 names a new `Deferred::MainPassTargets()`, but Deferred is lane A's file
  and that function is not in its headers. Lane A can add it and the table can call it.
- **A repeat `InitTargetFormats` / `InitShadowInputs` with the same formats does nothing; other formats are fatal.** D3D11 mode
  runs `State::Setup` again after a resize, so a plain "second call is fatal" would stop the game there.
- **The bound-format check at the opaque start is fatal (`report_and_fail`) and skipped in release profile builds.** It also
  fails when nothing is bound, which the old code tolerated with a warning and a skipped colour epoch.
- **A shadow view drawn in another depth format stays a `DclfInputChanged` tap error.** It is a defect check against the
  fixed format, not an observation; the shadow pipelines' format is never replaced by a view's any more.
- **The `GetDesc` shape reads on engine fields in DCLF stay** (ShadowCapture, ShadowCapability, Resources, FirstPersonView).
  After lane T a field handle's `GetDesc` answers the adopted description, the same as `Targets::Desc`, and it also works in
  D3D11 mode, where the set is empty.
- **Identify is only asked about tripwire handles.** ImageSpace, Skeleton and FrameBindings check `Tripwire::IsHandle` first,
  since a loaded texture or a lent CS texture can be a plain DXVK object in guest mode, and the set treats that as a caller bug.
- **Mip view targets:** ImageSpace names one as its source with `WithMip`, as before (the old SRV-format check is gone).
  Skeleton leaves their clears to the engine (it registered no pass for them before either).
- **Skeleton finds a resized target by its extent.** The field handles keep their pointers across a resize, so the clear and
  copy registrations, which bake the viewport and copy size, compare the texture's adopted extent at frame start as well.
  This is per-frame observation of the engine's shape and should become the set's adopted callback once one is exported.
- **EngineTextures and DynamicRing are not on `ExportForEngine` yet.** `GraphResources::ExportForEngine` / `ImageRegistry`
  are not in lane A's headers (b79bae815), so that part of B15 waits for them.

### Lane M lead, phase 3: ports' engine targets, Observed, D3D11 wrappers (decided by the agent, to confirm)

- **D3D11Wrapper re-points plain objects, not tripwire handles.** `GraphResources::MakeWrapper` gives the feature a Buffer.h
  texture, and `RetargetWrappers` (one call after the twins' `AdoptPrepared` in `Revision.cpp`) points its texture and views at
  the twin's bound version. CS's own D3D11 code (Deferred, the fog, DCLF's bindless lookups, buffer viewers) binds these on
  DXVK's raw context in guest mode, where a tripwire handle would need an Unwrap at every reader.
- **`Frame::Observed<T>` is a bound setting holding a serial**, with the values kept by serial until the adopted revision is past
  them. So it needs no change to the variable table and takes any copyable `T`. It is render-thread only.
- **Full-size engine targets take their extent from `DisplaySize`** in keys (Post Processing, FX runtime, Upscaling's depth
  copies and their render area). There is no `EngineTargetShapes` variable to depend on in lane A's headers.
- **Shadow map views that are not tripwire handles keep the Unwrap compare.** Physical Sky, the clouds and Skylighting name the
  directional shadow map with `Targets::Identify` when it is a handle. The view Deferred captures may be DXVK's own object,
  which `Identify` treats as a caller bug.
- **Upscaling's copies of the refraction normals and underwater mask are `Targets::Slot(ColourCopy(...))`**, not D3D11 imports.
  This needs lane A to back both copy targets.
- **Dynamic Cubemaps' capture target is observed** (`Observed<uint32_t>`), not a constant from Deferred: Deferred's main-pass
  targets are lane A's file.
- **Post Processing reads kMAIN's slot in single-target (native) mode too**, where it read `TonemapTargets::input`.
- **VL's mid-frame volume declaration keeps its `Invalidate`.** Lane A moves the declaration to setup and removes it.
- **Sun rays' `raysTargetFormat` setting is gone**: the format is the session's kMAIN RTV format.

### Residue lead, phase 6: A3 steps 7-8, B18, gated engine work (decided by the agent, to confirm)

- **One `UploadRing` (`Engine/Buffers/UploadRing.h`) with the chunk makers left to the clients.** The dynamic ring's chunks are
  still ORG GPUUpload buffers with DXVK wraps (`MakeBacking`/`EngineView`, which phase 3 may move onto ExportForEngine); the
  constants' chunks are dedicated latch blocks. The ring only shares the bookkeeping: current chunk, parking per frame, release
  behind `ReleaseAfterD3D11`, free list.
- **The engine's constant-buffer pools are not GPU buffers.** In graph mode a Map of one of the 146 pool buffers is answered
  from CS memory (two copies per buffer, swapped at each discard), in both modes, through `Tripwire::SetMapHooks` (was the
  guest-only `SetGuestMapHooks`), asked before the native service. Nothing on the GPU reads them except through the capture,
  which copies, so an ORG buffer would only add a slow write-combined read. Guest mode answers once the flush latch is in
  effect. Their D3D11 buffers are never written after that.
- **Known change:** guest D3D11 frames that still bind the engine's per-frame buffer (b12) read a stale buffer, for example
  Exponential Height Fog's D3D11 frames (A3, "frames that still run on D3D11"). They go with those frames.
- **`Flush::WriteBehind` went with `KeptWrites`.** CS's own dynamic constant buffers (State's permutation, shared and feature
  buffers) map DXVK again, inside a stage's window too, so the window census counts them again (about 40-55 maps a frame before
  the write-behind). A discard map adds no GPU work. They stop once no D3D11 path reads them. `FrameAnchors::SetWindowsClosedHook`
  is unused now; it stays, since FrameAnchors edits were kept to the foreign-step deletion.
- **`CS_ORG_ENGINE_CONSTANTS_CHECK` is gone.** It read back the D3D11 buffers, which are no longer written.
- **`EngineDraw` and `EngineWork` are new tap errors.** A draw the flush latch withholds is `EngineDraw` (it was a logged skip).
  In development builds any game-code draw or dispatch the latch did not withhold is one too: that is the census as a check.
  CS code through the native context is not counted. `CS_ORG_ENGINE_DRAW_TAP=0` only logs both. It is on by default; it exists
  because the mode smoke could not be run here.
- **Native immediate-context host waits stay** (NativeDevice.cpp: the slot ring's `AcquireSlotLocked`, `Commands::Run`, Map's
  wait for `lastUse`, staging updates and depth-stencil staging copies, `ImmutableTextureOf`'s transfer wait). They are D3D11's
  own Map and staging semantics for the engine's remaining readbacks and uploads. Removing them needs those uploads declared;
  the pool constant buffers, the biggest user, no longer go there.
- **Worker's site calls:**
  - Skeleton: a clear at a patched engine site with no pass is an `EngineWork` tap error and is dropped. Setup refusals only record
    why; "no clear program" and a missing clear or copy anchor are PortFatal. Engine-owned and unbacked colour targets keep the
    engine's call. Guest Skylighting's lent precipitation occlusion view keeps the engine's clear (its draws are D3D11 there).
    A remade target's clears and a copy of a texture remade inside the frame lag (dropped, ReasonLog) until the next Begin.
    The native context's own clears are not gated (they may be CS's).
  - Occlusion queries: a ring query outside a bracket, or a second in one bracket, is `EngineWork`; the slot answers its last count.
  - Bink: no tile draw reaches the engine in graph mode. Unregistered or not-yet-held ticks lag (decode into graph staging,
    draw nothing); off-thread, odd layout, no movie, not mode 1 are `EngineDraw`. Setup fails without a stage holding Present.
    A staging allocation failure is PortFatal.
  - EngineTextures' "left to the engine": unchanged. Scaleform's graph passes read those DXVK textures through
    `ResolveBinding`, so no D3D11 frame comes of them; the creation path is phase 3's.
  - ImageSpace: sampler, viewport and builder failures are PortFatal. "Effect not made yet", "no ORG state form yet", "no
    program" and "input not published" stay as not-yet states: the engine shape and publishes rebuild the graph, and the step's
    draw is a `NoPass` tap error meanwhile.

### LLF lead, phase 6: A13 culling and D-1 (decided by the agent, to confirm)

- **Culling at the Prepass anchor, one registration for the session.** The client's key is empty, so it never registers again;
  the cluster buffers follow the display size by versions. Its passes take orders -300 to -298, so the Prepass readers (the fog)
  come after them. `kMaxNames` went to 11 for the Prepass step.
- **`Segment::LightCulling` is removed, not kept unused.** The segment values after it shift by one; every epoch number is
  `EpochOf(segment)`, built together, so nothing persisted depends on them. `CS_ORG_TEST_CONTRIBUTOR_EPOCH` now defaults to
  `cs.anchor.prepass`. The foreign-step machinery (`FrameAnchors::SetForeignPasses`, `HoldsForeign`, `TapForeign`,
  `Step::foreign`, DCLF's `Api::ForeignEpoch`) has no user now and is left for the core lane to delete.
- **The culling commit writes the last staged values every run.** The light copy is not gated, so a run without a fresh
  `SetFrame` copies the last frame's lights instead of an unwritten latch region. Before the first `SetFrame` no dispatch opens.
- **The parity check compares one frame later.** The graph culls at the Prepass anchor, after LLF's Prepass hook, so the
  parity frame's D3D11 culling is compared at the next frame's hook, before that frame's anchor runs.
- **No lag guard for the first frames.** Until the first revision adopts the culling's registration, the Prepass tap does not
  cull, and the light buffers hold undefined contents. This lands before the world's first frame today; the held-queries lead
  (lead-p4-held) owns a general answer.
- **The cache reads DCLF's drawn slices rather than being a target DCLF draws into.** The cache is R32F and downsampled by a
  power of two (LocalShadowCopyCS tree-reduces), so DCLF's views cannot draw into it without new placements, formats and
  resolutions. One copy per newly drawn light remains. A declared array DCLF's local views draw into is possible after phase 5
  makes the shadow views declared clients.
- **The cache follows DCLF only in graph mode.** `LocalShadowsFollowDCLF` is DCLF loaded, `CS_DCLF` not 0 and graph mode.
  Plain D3D11 with DCLF loaded keeps the rotation, since DCLF draws nothing there; it was blocked before.
- **In range means drawn or on screen.** Without the UpdateCamera hook, a light DCLF drew or whose sphere is in the view
  frustum sets `lastEligibleFrame`. Eviction and admission read it as the engine's range test before.
- **Only covered views count.** A view the engine draws itself (not covered) is skipped, since it comes from the engine's own
  selection, which may differ from DCLF's. Its light keeps its cached slot.
- **The matrices are the engine's descriptor's.** DCLF draws with the engine's camera (its parity checks compare them), so
  `lightTransform` and `shadowBiasScale` are read as before. DCLF's predicted slice is the source slice.
- **The settings note in place of the block.** In graph mode the menu says DCLF chooses the lights and the cache keeps their maps
  (`feature.light_limit_fix.local_shadows_follow_dclf`; en.json regenerated, the old key pruned).

### Held lead, phase 4: A9 and B3 (decided by the agent, to confirm)

- **One query, three kept beside it.** `FrameAnchors::Place(id, anchor)` returns `{stage, held, scheduled, built, generation, lag}`
  and replaces `Scheduled`, `StagePassed`, `InStage`, `HeldNow`, `CanTap`, `TapMayLag` and `InBuiltGraph`. Anchor `Count` means any
  open stage that holds the client (the old anchor-free `CanTap` and `TapMayLag`). `Stage(segment)` replaces `StageOpen` and
  `StageAbsent`. Kept, since they answer another question: `HeldByStage`, `NamedByStage`, `StageStepsAt` and `FirstGraphInstance`
  (the stage layout, fixed after init), `StagePlace` (which of several clients takes a clear) and `StageInputs` (a write).
- **A registration matches as the run's commit does.** `scheduled` is by id for a client with a generation, else the exact
  registration. The old `HeldNow` matched every client by id, so a client without a generation whose schedule still holds an
  older registration now lags (MenuSteps, the Skeleton clear places, DCLF's named families). The run would not commit it anyway.
- **At an anchor a stage holds wholly, Place needs no open window.** It answers for that stage until it ends this frame, then
  `Lag::Passed` (still with `built`, for a Bink tick before Begin). `HeldNow` needed the window open, `Scheduled` did not.
- **`matchesById` is gone; a nonzero generation means by id.** Only `DeclareClient<Key, State>` sets it. Every client that set
  `matchesById` is now a client with a state: Sun Rays, the post chain, FX, HDR, Physical Sky's AP shadow and composite,
  Volumetric Clouds, ViewAtmosphere's composite, Screenshot's clean copy, Scaleform, plus Upscaling's frame generation inputs and
  Screenshot's capture copy (for their wanted/made pairs).
- **The newest commit runs for an older held registration, so commits read the held state.** Sun Rays, the post chain and FX
  captured the newest registration's state in their commit; a schedule that still held an older one committed the newest's
  values (the post chain committed nothing). They now commit what `Held` found that frame.
- **`Held` for schedule-held work, `Adopted()` for the rest.** HDR, Physical Sky, Volumetric Clouds, ViewAtmosphere and
  Screenshot's clean copy read the adopted registration at hooks outside their tap, where a shared anchor's window may not be
  open, so they use `Adopted()` (the state of the registration the adopted revision holds, reset when the client is removed).
- **`active` is not merged into `declared`.** `active` is per run: at a shared anchor (EngineDraws, FrameGeneration) it says whose
  run this is, so one family's run never commits another's. `declared` is read once at Begin and cannot say that. Folding it in
  needs `Run` to name its client; left for A5 phase 5. `segmentOnly` stays too: a client a stage names can still have passes at
  an anchor with its own epoch, so it cannot be derived from `NamedByStage`.
- **The raw-registered `Scheduled` callers ask `Place`, but are not moved to `DeclareClient`.** Cloud Shadows, Deferred Clears,
  Upscaling's evaluate and hudless clients and DCLF's families (B19) keep their registration. Moving them changes when they
  register (the first revision instead of setup); a `declared` predicate for each is a follow-up.
- **States are pruned by `Held`.** A `Held` that finds generation g drops the states older than g. A state is never dropped
  when the client is removed, since a lagging schedule may still hold it.
- **ImageSpace's four `*Lagging` checks stay.** They compare the engine's adopted key with the newest built key, which `Place`
  does not answer. B21 (step groups) is the place for them.
- **Skeleton's `group.generation`/`hostedBuilt` stay.** They match DCLF host builds, and the groups register raw.

### ImageSpace lead, phase 4: B21, A4, A2 and B20 for ImageSpace (decided by the agent, to confirm)

- **A shared `StepGroup<Build>` template, not recipe rows.** `RecipeTable.h` is a constexpr, ORG-free table of stages, steps and
  client ids. The step groups' passes come from the engine's shape at run time (luminance and SAO levels, the heightmap bound, the
  covered effects), so they cannot be constexpr rows. `StepGroup` (`WorldSteps.inl`) declares one `DeclareClient<Key, Build>` per
  part, builds the group once per key and keeps the last four builds. The four `Key`/`KeyOf`/`Build*`/`Declare*` sets keep only
  their own steps; `BuildStepsOnce`, `DeclareStepClient` and the description caches are gone.
- **Each registration's state is its build** (a `StepTable`: entries, rows, specs, made flags). Hooks match a draw with
  `StepGroup::Find`, against the build `Held` finds, never the newest. Rows callbacks and pass makes read their own table. The four
  `*Lagging` checks are one rule: a draw the held build has no pass for is lag while the newest written variables give another
  key (`StepGroup::Ahead`), else a `NoPass` mismatch.
- **Held at any open stage (`Anchor::Count`) for the world, chain and start groups; at the ImageSpace anchor for the menu
  composites.** This keeps what each group asked `Place` before.
- **The key function finds a kept build.** A registration with a state is adopted by asking its key again at Begin
  (`Declare.cpp` `NoteBegin`), and the key function builds on a miss. Without the kept builds that would rebuild the older key each
  Begin while the newer registration lags.
- **Rows from the description read at Begin; `kRequestFrames` and `requested[]` are gone.** A drawn step whose row is off is lag
  when the frame's description at Begin has it on (the build's key lacked it), else `NotInDescription`, which is fatal in development
  builds. The world scopes keep the fields their rows read (`NoteWorldBegin`). `CheckEndOfFrame` no longer accepts the live post
  fields at the end of the frame. The VL composite's group is now always on (gated), since its comment said Begin's fields miss it.
  Risk: an engine state that changes between Begin and its draw now stops a development build. `imagespace-transitions` and
  `post-effects` check for that.
- **Draw matching by occurrence stays.** The minify levels, the luminance levels, the slot 6 and 7 inner draws and the water
  update's ripples, heightmap steps and flow copies are one engine loop drawing the same effect several times. The description can
  say how many, not which draw is which. Naming them by bound target would need the target identification the variants avoid.
- **The shape is raised at events, not polled (A2).** `RaiseShape` writes `EngineImageSpace` at Setup, at each Begin only until the
  engine has made its manager with the four blend states (no hook of ours sees that creation), at the first sight of each effect
  object (`EffectOf`), and when the hook that runs a part of the shape sees it change: the HDR effect (luminance levels),
  RenderSSAO (SAO levels) and the water update (displacement size, flow map, heightmap bound). The variable keeps its struct. The
  composites' programs are wanted when the shape changes, not at each Begin. The engine's own creation functions for effects and
  the flow map (SE 31234) have no known AE ids, so the hooks that use them stand in for creation hooks.
- **B20: ImageSpace's passes already use `AnchorPasses` (`MakePass`, the copy-backs, the capture copy).** The one hand-written pass,
  the `CS_ORG_ADAPT_READBACK` diagnostic's `ReadbackPass`, stays: it copies versioned slots into a buffer, and `RunReadback` takes
  plain resources only. It also reads back outside `RunReadback`; moving it needs a slot form of `RunReadback::TexturePass`.
- **Left to other leads:** `ClearFirstVersion` (Birth, lead-p4-birth) and the "pass is left out" policy (`MakePass`, lead-p6-residue)
  are unchanged; their code only moved into the builds.

### Holds lead, phase 6: A3 steps 3-5 (decided by the agent, to confirm)

- **Setup's build point evaluates the declarations, but adopts no variables.** `Frame::Revision::EvaluateAtSetup` runs in
  `Engine::PostSetup` after the setup batch and before `BuildPending`. It builds the declared programs (a setup wait, as
  `JoinSetup`), evaluates every declaration on the newest values, and adopts the registered clients' states. The variables are
  adopted by the first revision as before, so its input producers still run. The stage build needs it returns are dropped:
  `BuildPending` builds them inline.
- **Every client key now runs at setup.** About 35 declared clients register before the first frame instead of at frame 1. A key
  that reads game state that does not exist at setup could fail there.
- **Ports observe at setup through `Revision::OnSetupBuildPoint`.** Terrain Blending observes the two depths and SSS imports the
  engine targets, so the setup build holds their clients.
- **No new stages.** A frame kind without a stage for a client draws nothing in graph mode: the post chain, FX and Sun Rays in the
  main menu, and a menu scene's tonemap after the post stage. A Tonemap step in the UI stage needs the menu ImageSpace client split
  around the tonemap (B21), so it waits for lead-p4-imagespace.
- **The post chain and FX are not seeded at setup.** Their keys need one observed tonemap, since the effects' `Update` reads game
  state (DoF's focus target). Until then the frame draws no chain; the main menu's tonemap observes it before the first world
  frame.
- **A lagging stage draws nothing.** For every client here, a frame whose stage runs an older schedule (`Place(...).MayLag()`) draws
  nothing this frame, logged once. A stage that never will is fatal. Terrain Blending's blended depth keeps the last frame's.
- **Terrain Blending's and HDR's clients no longer follow their on switch.** Removing a client unregisters it at the revision start,
  before the adoption that switches the setting, so the old code ran D3D11 frames in between. Now they stay registered, and only an
  enabled frame runs their anchor.
- **Color Grading:** the curve preview draws the ramp on the graph and reads it back (`RunReadback`) every run. The LUT and output
  export is not offered in graph mode.
- **Lens Flare's debug switches build no flare**, so Composite sees it off. D3D11 instead keeps last frame's textures.
- **Physical Glare's kernel view shows a hint in graph mode.**
- **An unloaded LUT is no LUT.**
- **DoF Target Focus without a target** now gives whole constants (a 1 mm lens, no near blur), so the graph runs DoF as near
  pass-through. D3D11 still skips the draw.
- **FX reload after the plan:** the run writes the plan's last values, from the compiled effect it keeps, until the next plan is
  adopted.
- **FX and the post chain draw the game's tonemap** in a frame without a held build.
- **Sun Rays without terrain heights** (before a heightmap) bind none. `EnableTerrainShadow` is off then, so the raymarch reads
  nothing.
- **The no_d3d11 check counts `d3d11_before_graph` too.**
- **Worker: Exponential Height Fog:**
  - The first client binds only the engine depth (`FirstObservation`), so a fog switched on has a client before its first Prepass.
  - New shader flags (256, 512, 1024, 2048) gate the terrain heights, the cloud cubemap, the sky LUT and the cloud shadow volume. The
    heights and the cloud shadows are absent before they are written.
  - LLF's local shadows on D3D11 and scene depth that is not the graph's are fatal.
- **Worker: Cloud Shadows:**
  - Engine-drawn cloud faces stay on D3D11 and are counted. They belong to DCLF coverage (A3 step 6).
  - A full scratch array is fatal.
  - Ops left for another anchor, or an anchor not scheduled, are fatal. A lagging stage is fatal there too, since a lost layer op
    corrupts later frames.

### Birth lead, phase 4: B5, B6, A12, B20's clears (decided by the agent, to confirm)

- **`Birth` lives on `GraphResources::Options::birth`** (`Frame/Birth.h`), so the declaration calls keep their shape. Its
  factories are functions (`Birth::Zero()`, `Value({...})`, `Fill(fn)`, `Undefined()`). Unset means the old rule: History colour
  textures are born Zero, the rest Undefined. Only declared textures take a birth; a plain twin with one is fatal (WhyUnborn).
- **How the lane makes a birth.** Graph-only versions: the uploader zeroes them or, for Value and Fill, a new ORG entry
  (`QueueTextureFill`, OMS `TextureContents::Fill`) copies a staging buffer the size of the texture's footprints. Guest exports:
  DXVK's wrap takes Value/Fill as its initial data (zeros otherwise, as before). Native colour exports: the uploader, zero even
  when Undefined (lane T's floor). Depth and multisampled textures can only be Undefined; anything else is fatal at declaration.
- **The first version is made again on the lane.** A declaration whose birth writes asks the first revision for a renewed version
  at the same extent, so version 0 (made at setup) is never bound by a client. Exported version 0 is zeroed by its wrap anyway,
  which is why Dynamic Cubemaps' zero upload could go without a declaration.
- **Value is encoded per format** (`Frame::EncodeTexel`): float, unorm, snorm, sRGB, R11G11B10, R10G10B10A2 and small uint
  formats. A uint value above 2^24 does not survive the float; Skylighting's all-ones bitmask uses `Birth::Fill`.
- **`Written` replaces the frame-based `FirstFrame`.** `FirstFrame` is now `!Written`: true until a client's executed callback
  calls `Frame::NoteWritten(texture, version)` with the version its commit read. Every reader of FirstFrame was converted (EHF,
  Color Grading, Lens Flare, Physical Glare, SSGI's went with its clears, Skylighting's went with its resets).
- **Physical Glare's `fresh` became a per-build bake flag** (`bakedThisBuild`): a new build may bring new programs, which is per-build
  behaviour, not first contents.
- **SSGI's history resets are new versions.** Its textures depend on every configuration setting and on a new `historyResets`
  setting; the render thread raises it from `queuedResetHistory` (loading screen, shader reload) at Prepass. With SSGI off its
  client has no passes. Risk: a client with no passes may be refused by the anchors.
- **Skylighting's resets are births.** Each volume is born in ResetSkylighting's state; the reset clears, `resetsSeen` and the
  reset gate entry are gone. `mapCleared`/`committed` stay: they are about the occlusion depth map DCLF draws, which is a plain
  depth twin that cannot be born. Open.
- **Kept as explicit per-run behaviour:** Terrain Blending's `TerrainDepthCleared` (a per-frame depth reset), HDR's `uiCleared`
  (ClearUIBuffer's per-frame answer), the clouds' `ClearVolumetricToNeutral` reset, EHF's far and debug clears.
- **Not converted, open:** Terrain Shadows' heightmap upload and Glints' noise fill still use `FirstRun`. The heightmap is a new
  plain twin per heightmap (it needs a declared texture on `HeightMap` with a Fill that finds the staged bytes, or `PublishTexture`
  of an uploaded image); the noise is a GPU compute fill, which a CPU `Birth::Fill` cannot express without porting the noise.
- **B20.** `AnchorPasses::Clear` is the one clear; `ClearOf(name, twin, value, gate)` and `MakeClear(clear, gate)` for hand-made
  pass lists. The twin overload of `GraphPasses::MakeUavClear` is gone; `UavClearPass.h` is used by AnchorPasses and its self-test only.
- **A12.** A second resource under a published id is fatal (`PublishEntry`), and `Resource::Register` refuses another resource or
  resolver under its id (ORG `RenderGraph::RegisteredResource`, which asks no provider). An added id still asks the builds that
  looked it up, as before. Known sites that republish another resource, now fatal: LLF's cache on a slot-count change
  (`ORGLocalShadows.cpp`, `Impl::Adopt`), ImageSpace's `PublishFlowMap` (`StartSteps.inl`), possibly DCLF's `FrameBindings.cpp`
  engine-depth import if it is not a slot. HDR's twins now live for the session so its rebuild no longer republishes.
- **B6.** `ExistsWhen` keeps setting ids, not references, since Post Processing replaces its sub-feature objects. Sun Rays keeps its
  rounded-up half size by hand. Probes left: Terrain Shadows heights (published only once a heightmap exists), Terrain Blending's
  depth choice in EHF, LLF local shadows, `ORGDeferredComposite` (a view picks between two ids), DCLF's.

### Fragments lead, phase 5: A5 steps 4-5, B10 (decided by the agent, to confirm)

- **The fragment source posts through the core.** `Revision::RegisterFragmentSource(FragmentSource)` returns an id. A source
  posts with `PublishFragment(id, Fragment)` into a latest-wins slot the core holds, and the core calls back `taken`, `adopt`
  and `drop`. The plan's `take()` became the core's own post. Versions are `Revision::FrameVersionsFor(id)`. Other callbacks:
  `drawing`, `afterAdoption`, `frameStart` (native mode's build point at Begin), `drawsWorld` (`Description::sceneDraws`, was
  `dclfDraws`) and `epochFailed`. Up to `kMaxFragmentSources` (4) sources, each with two revision slots.
- **A source's segments are the lane-only build epochs.** The runtime reads `Revision::FragmentSegments()` at Initialize, so
  DCLF registers in `DrawcallLimitFix::Install`, before `ConfigureFrame`. When DCLF does not install (hook conflicts), no
  epoch is lane-only. Graph mode requires DCLF, so that case does not run.
- **`kSceneIdleFrames` became the source's `drawing()` state.** A revision with a source's epochs waits for its fragment only
  while the source's epochs ran in the last frame (DCLF: `SceneRecordedFrames` moved since the last Begin). The old rule
  tolerated 12 idle Begins. Now one frame without DCLF's epochs adopts the revision without DCLF's scene. Its next scene is
  then recorded on the adopted versions. That also happened before, after 12 frames.
- **One starvation rule.** `Frame::StarvationWatch{what}` (`Frame/Starvation.h`) with `NoteInput()` and
  `NoteMiss(why, segment)`. It raises `TapError::Starved` after 16 misses (4 frame slots x 4). It replaces
  `kRevisionStarvedAfter`/`askedAgainSinceInput` (the revision's watch), `sceneMissesSinceInput` (one watch per source) and
  `kRestageStarvedAfter`/`restagesWithoutInput` (DCLF's `restageWatch`). DCLF's restage watch still takes a new build state
  serial as a new input, as before.
- **One recording request service, B10's first half.** `Frame::EpochRecordings::Request(host, epoch, hostData,
  buildGeneration, copy, what, done)` classifies the outcome for every caller: a drop, or a recording on another build, is
  `Dropped`; any other error is fatal in the service. FrameAnchors' `RequestDeclared` and DCLF's `RequestRecordings` use it.
  DCLF's recording errors were a logged warning that failed the fragment and were never asked again. They are now fatal.
  A refused post (async epochs off) still returns false to the caller, as before.
- **Not done in B10:** the per-epoch caches stay where they are. FrameAnchors keeps `SegmentRecording` entries and
  `AdvanceRevisionRecordings`; DCLF keeps its own `RevisionAssembler` of shapes and recording fragments. One service that
  owns the cache too needs DCLF's shape fragments and hosted-view variants mapped onto per-slot requests. That is the next step.
  (Done since: see "Coverage lead, phase 5: B10 rest" below.)
- **`DclfPassCuts` is DCLF's setting.** It became `CS::Setting<std::uint64_t>` bound to `DCLF.PassCuts`, like Effects11's
  `shaderCacheClears`. The core row is gone. `Vars::Raise` has no caller left.
- **`TapError::DclfInputChanged` is `ClientInputChanged`.** The name text is "client input changed after it drew". DCLF's
  message starts with "DCLF:".
- **The violation warning text says "the menu"**, not "the DCLF menu".
- **What the core still names:** `RenderGraphRuntime::Segment` lists DCLF's epochs; their labels ("CS DCLF: ...") and the
  segment census's names stay. So do the diagnostics `SkipDclfEpochs`, `HoldDclfSubmissions` and `DropDclfLists` (A11 test
  paths), and comments on DCLF's frame values and schedule (`ConfigureFrame`). Moving the epoch list into the fragment source
  is a later step.
- **A4 (shader reload) was not done here** (done by the A4 lead below). The swap still built every graph inline, and DCLF's wait stayed. `AdoptScene` and
  `SupersedeForSceneWait` became `AdoptFragment(id)` and `SupersedeForFragmentWait(id, why)`, used only by that wait. What
  it needs: live pipeline slots are read through by recordings (`LivePipelineSlot::Peek`, no reference held), and
  `PipelineRegistry::Publish` swaps them in place. So frames cannot keep old pipelines past a publish, and a staged build
  cannot bind the staged pipelines before it. A4 needs pipelines as versioned inputs: either recordings hold their payloads
  and the registry builds staged builds against staged payloads (OMS `PipelineRegistry` and ORG), or one version of each
  slot per revision. Then the ShaderGeneration revision stages every build on the lane, waits for the members' recordings and
  each drawing source's fragment, and publishes at adoption. Then delete `WaitAfterPipelineSwap`, `WaitIfPipelinesSwapped`,
  `SwapStagedPipelines`, `AdoptFragment` and `SupersedeForFragmentWait`.
- **A3 step 6 is not done.** What remains:
  - coverage per epoch in `RecipeTable.h` stages (an `owned` field per DCLF epoch), checked at the frame's end like taps;
  - `FrameDriver.cpp` (DCLF) `DecideCoverage` exemptions: claimed epochs only, `NotDrawingYet`, load screens;
  - `Coverage.cpp` gated epochs, and forward/first person without batches;
  - `OcclusionNeedsEngine` and the LoadHold re-entry, `MainViewPoints.cpp` colour-capture skip, the CPU site not live;
  - per-draw `TryLatchDraw` refusals (seven files) move to classification;
  - `CheckSceneVersions` (`Snapshot.cpp`) becomes `TapError::StaleRecording`. Not done, because with the one-frame
    `drawing()` rule a revision adopted without DCLF's scene leaves DCLF's installed scene on old versions until its next scene.
    That needs the scene to be a required member, or the check to skip the frames before DCLF's next scene;
  - `IndirectDraws::failed` still hands drawing to the engine.

### Coverage lead, phase 5: A3 step 6, B10 rest (decided by the agent, to confirm)

A3 step 6:

- **Coverage is declared in the recipe.** `Table::Epoch::covered` (helper `Covered(Segment)`) marks the fragment source stages'
  own epochs, and `Table::CoveredSegment(segment)` looks one up. `Valid()` allows it only on a non-CS stage's Graph epoch. DCLF
  checks it at its frame start (`DecideCoverage`), not at the frame's end like taps, because claims are decided there.
- **Named engine states** (`DCLF::Draws::EngineState`), each from what the Begin adopted: `NotDrawing`, `LoadScreen`, `Off` (the
  gate class, shadows or the view toggled off, or the view not registered; this replaces "gated epochs" and "a CPU site that is not
  live"), `NoShape` (the adopted scene has no shape for the epoch) and `HostEngine`. While DCLF draws, an uncovered covered-segment
  epoch with none of them is the new `TapError::Uncovered`. So is the frame-level claims failure (was `report_and_fail`; a lost
  device and a stopped graph stay fatal as before). Release builds keep the old outcome: the engine draws that work.
- **Forward and first person without forward batches are no engine state.** The adopted scene has them whenever its build draws
  forward directly, so a missing set leaves the epoch uncovered: a tap error. Forward was already fatal through the claims.
- **The load screen stays a state** read from the engine's flag at DCLF's frame start (`FrameView::IsLoadingScreenUp`), not
  through the revision. Its frames have no claims.
- **Colour capture without bound targets** (`MainViewPoints.cpp`) is `TapError::Uncovered` while DCLF draws.
- **Occlusion maps.** A map `OcclusionReady` took for the shadow epoch that the epoch did not draw is `TapError::Uncovered`. A map
  not taken (off, not in the adopted shadow shape, its phase still joining) stays the engine's, and the LoadHold re-entry for it
  is unchanged. Moving that re-entry to the adoption point is open.
- **A family's client not registered yet** is left as it is: registration happens only at a revision's adoption, so it already is
  an engine state switched there (comment on `FamilyClient::Registered`). Making it fatal after the first world frame is open.
- **Per-draw latch refusals are fatal, not moved to classification.** The seven families' refusal logs became
  `Family::RefusalLog`, which raises `TapError::Refused` in graph mode for every report (each reason is still logged once).
  Classification can't see the bound shaders, states and constants these checks read. A refusal that turns out to be routine
  shows up in the first test that hits it, and then needs its own classification-time rule. Decal occlusion's skips ("answers
  visible"), the sun query's skips and Sky's cloud-layer refusals are included; Precipitation's "targets past the two outputs"
  note is not, since the draw is still taken.
- **First person's view coverage** (the feature's `covered` function deciding the view stays the engine's) is not changed.

Item 2, `CheckSceneVersions` (the fragments lead's blocker):

- **A fragment source starts and stops drawing at the adoption point.** New `FragmentSource::left(revision)`: a revision with the
  source as a member adopted without its fragment calls it inside the adoption point. DCLF then stops (`FrameView::StopDrawing`):
  it withdraws its claims, notes every claimed member as left for the primary cull, and `NotDrawingYet()` is true again. Its next
  covered frame installs the claims whole, as the first one did. `EverDrew()` keeps the "after it first drew" input checks
  (target formats, main resources, shadow format) on.
- **`drawing()` is "DCLF draws and its epochs ran last frame".** So a revision waits for DCLF's scene only while DCLF draws, and
  otherwise leaves it and stops it.
- **`CheckSceneVersions` is strict.** It runs after every adoption (`AfterSceneStep`, once per new scene or adopted revision). A
  stale scene covers nothing (`sceneVersionsStale` in `DecideEpochCoverage`). While DCLF draws it is `TapError::StaleRecording`.
  While it does not, it is logged and DCLF waits for its next scene.
- **The cost:** after a frame revision adopted in a frame without DCLF's epochs (load screen, map, a freeze-background menu), the
  engine draws the world until DCLF's next scene, about 1 to 3 frames. Before, DCLF drew its stale scene. In native mode those
  frames draw what the engine's draws reach there, as before DCLF's first frame. To confirm.

Item 3: `IndirectDraws::failed` is gone. Its only setter, the main pass's resource setup, is `PortFatal`. `Failed()`,
`ViewPointContext::failed`, `coveredOnFailedGraph` and `CoverFailedGraph` went with it.

Item 5, the core naming DCLF:

- **Labels come from the source.** `FragmentSource::labels` (label, inputs label per segment), read through
  `Revision::FragmentSegmentLabel`. DCLF registers its old label texts, so traces and logs read the same.
- **Renamed:** `SkipFragmentEpochs`, `HoldFragmentSubmissions`, `DropFragmentLists`, `Owner::Fragment`, the recipe line's
  "declared by the fragment sources' stages", the segment census's "source shadows/depth/main/forward+1st".
- **What still names DCLF in `src/RenderGraph` (outside comments), and why:**
  - the `RenderGraphRuntime::Segment` values' comments and the recipe's commit texts ("just before DCLF's Z-prepass"): they
    describe engine points, printed in the recipe;
  - `Engine/Skeleton/Skeleton.cpp`: `HostInfo::dclf`, `DCLF::EpochsRun`, `DrawsShadowViews`, `InDepthPass` (clears and copies
    hosted in DCLF's epochs). That is lead-p6-residue's file now; it needs a "hosted in a fragment source epoch" query;
  - ImageSpace, Scaleform and Bink use `DCLF::GpuTextures` and `DCLF::Family` (texture binding and geometry latching). Those are
    shared utilities that live in DCLF's folder; moving them to `src/RenderGraph` is a separate step;
  - `Engine/Native`: `RequireDclf`, `CS_DCLF` (graph mode requires DCLF, D-3); `RtSampler` attributes `DCLF::` symbols;
  - `RenderGraphRuntime.cpp`'s pass-prefix attribution table row `cs.dclf` (one row per feature);
  - `Testing/OrgCommands.cpp`: `CS_HAS_DCLF` and `DCLF::Draws::asyncComputeOverride` (test commands);
  - `BufferRefs.cpp`'s `CS_DCLF_REF_CHECK` and `DxvkImageImport.cpp`'s census text.

B10 rest (worker):


- **One cache type, owned by the service.** `Frame::EpochRecordings::Cache<Key, Keep, AfterDrop, Payload>`: the caller's key
  (hash plus `operator==`), entries of one or more requests that settle together, an LRU keep count, pruning of settled entries
  on builds outside the caller's `Builds{attached, staged}` at each `Add`, "a kept failure is fatal" in `Find`, and the lock
  the host's thread completes under. `Post` goes through `Request`. Each caller owns its cache instances. The service holds no
  registry of them.
- **FrameAnchors** keeps one cache per stage (key: slot, versions serial, fields; keep 6 x frames in flight; `AskAgain`). No
  behaviour change, except two texts: the kept-failure message is now the service's "the recording of ... failed" with the same
  description fields, and the attached build for pruning is `BuildGeneration(epoch)` rather than `EpochRecordingCurrent`, which
  also needs the epoch to have a graph. Both are the same whenever an epoch has recordings.
- **DCLF** keeps one cache per epoch. The key is the exact input fragments (versions, shape, hosted shapes), so it matches the old
  "requires exactly" check. An entry holds every variant of the epoch's recordings, and its payload is the `RevisionFragment` the
  revision slot holds. So `RevisionRecordings` in the slots and every reader (Coverage.cpp, `RevisionOf`) are unchanged.
  `PendingRecordings`, `RecordingCompleted` and `failedFor` are gone.
- **The drop rule stays per caller** (`AfterDrop`). FrameAnchors asks again at the next request. DCLF keeps the drop until its
  inputs change, as before, because asking again on the same build would record the whole epoch again only to be dropped.
- **DCLF keeps 2 entries per epoch** (the current inputs and the last). Before, it held only the current fragment and the failed
  inputs. Now the last inputs' recordings stay until the next new inputs or a build change, so inputs that come back (a gate class
  toggled off and on shares the same shape) record nothing new. That can hold one more set of an epoch's recordings for a while.
- **DCLF prunes on the builder thread** with `Builds{epochBuilds[segment], 0}` from its posted inputs, since `StagedGeneration`
  and `EpochRecordingCurrent` are owner-thread calls. A build change also changes the versions fragment, so old keys never match.
- **Small DCLF changes:** `recordingsOutstanding` counts entries, not requests (only read as non-zero). A refused post now fails
  the unnamed fragment instead of leaving it pending, and the entry is erased so the next make asks again, as before. A request's
  log name gets " (request i)" only when the epoch has more than one variant.

### A4 lead, phase 5: shader reload as a revision input (decided by the agent, to confirm)

- **Pipelines are versions, pinned per graph build.** OMS `LivePipelineSlot` holds immutable `PipelineVersion`s (payload and
  registry generation). A reload publishes each rebuilt payload as a new version at the next `Generation` when its last rebuild
  is in (`Service`), off any safe point; `Publish` and the staged set are gone. Every graph build pins the generation it is made
  at (`RenderGraph/PipelineVersions.h`, extension `cs.org.pipeline-versions`). The pin reaches the recordings through a new ORG
  API, `RenderGraph::SetRecordingTag` / `RecordingContext::Tag`, which every recording list of the graph holds. `ComputeProgram`
  and `RasterProgram::Pipeline(recording)` bind the version at the pin (`CurrentPipeline()` for a raw list submitted at once).
  Chosen over host data (it reaches Prepare, not Record) and over per-pass capture (38 sites).
- **No per-bind retention.** A recording does not `Retain` each payload. The pin its list holds keeps every version it may bind
  in the slots, and `Release` drops a version only once no live pin is older; its payload then retires through the retirement
  clock (`Retirement::SubmittedPoint`, `Clock()->Completed()`), no longer the host's retired callback.
- **The revision.** A published reload sets `ShaderGeneration` to the new registry generation (not adopted + 1) and asks for every
  graph (`Builds::Ask{all}`). Its input producer (`PipelineVersions::Install`) is ready once every epoch's attached or staged build
  pins it. A new generic `InputWait::pipelineGeneration` makes every fragment source a member of the revision and requires a
  drawing source's held fragment to be on builds that pin it. A fragment on older builds is kept and waited past, not dropped
  and not a starvation miss: the source records again once its rebuilt graphs are staged.
- **`FrameVersionsFor` names a member revision that changes no version** (its serial, null versions = the adopted ones). Before,
  a source was a member only when the revision had versions.
- **Non-staged epochs switch early.** Live epochs' builds attach at a poll, so they bind the new versions a few frames before the
  revision. CS stages and DCLF's epochs switch with it. A revision already in preparation when the reload publishes takes the
  staged builds that land meanwhile.
- **DCLF's scene is held while the graphs rebuild.** Its fragments are for the reload's revision, so frames keep its adopted scene
  until the rebuilt graphs and its fragment are ready. Frames flow; the scene may lag by the graph build time. The render thread
  used to stop for that time.
- **DCLF's draw pipelines** keep following the registry's newest generation (its sets hold their payloads), so they switch at its
  next scene after the publish, normally the revision's fragment.
- **Host-own builds** (owner thread, no build lane) count as attached at their build. A refused one would mislabel the gate; it
  stays safe, since nothing frees what a pin binds.
- **Graphs no host built** (`GraphInstance`) carry no pin and bind the newest version. A kept recording of one would not be
  protected; none is known.
- **Deleted:** DCLF's wait (`WaitIfPipelinesSwapped`, `WaitAfterPipelineSwap`, the `frameWaiting` gate override, the counters,
  the report's "shader-reload waits" and the capture's `adopt_wait_us` column), `AdoptFragment`, `SupersedeForFragmentWait`,
  `SwapStagedPipelines`, `PipelineSwaps`, `BuildManager::SwapPipelines`, `Retirement::PollHost`, `DrawPipelines::BuildsInFlight`.
- **The shader-reload tests** also refuse "shader-reload wait" and "graph built on the render thread after the first world
  frame". `ExecuteEpoch`'s first-build warning has the same text, so a run that hits it fails there too.

### Misc lead, phase 6: A13 presenter, A4 waits, cloud maps, B16 (decided by the agent, to confirm)

- **The presenter contract is `Presenter::Prepare` / `Presenter::Adopt`.** Prepare answers a revision's change with a
  replacement serial, or "at the adoption" (DXVK: a colour space change alone, which HDR Display sets; native: the same with no
  presentation layer). PresentationRevision has no mode branch left. A DXVK without `dxvkRequestSwapchainRecreate` or
  `dxvkSetSwapchainTornDownCallback` is fatal at Prepare; the trampoline is registered there too, so no serial-0 path is left.
  D3D11 mode (no revisions) still asks the presenter at once.
- **Native resize is not a revision input.** The engine's ResizeBuffers records the size and queues the swap chain resize on
  the submission thread after the queued presents; the swap chain keeps its own size, and the present copies the smaller extent
  until kFRAMEBUFFER's new version is adopted (design 3.4). Folding the size into `PresentationConfig` would need DXVK to defer
  the engine's own ResizeBuffers, which it cannot. The render thread still takes `presentLock` briefly, as QueuePresent does.
- **The submission thread still waits for the GPU before it resizes or replaces the swap chain,** with no timeout; a failed wait
  marks the device lost. Swap chain images cannot change while presents use them.
- **No hand-made present copy.** Before the graph tracks the startup back buffer, the present presents nothing (no image is taken).
  A refused ledger copy is fatal, as it already was for kFRAMEBUFFER. A ResizeBuffers failure on the submission thread is fatal.
- **PrimeStages asks for the UI stage's first recordings and returns.** Each slot switches its recording in at the first Begin
  that finds it done. If the startup logo's first frame comes before the recording, that frame has no UI stage (blank, not
  D3D11).
- **Cloud maps: the key is the step set, observed (`Frame::Observed<std::shared_ptr<const StepSet>>`).** The managers still
  describe their steps per update; declaring every step at setup needs file textures read bindlessly (shader layout change), so
  a changed file texture or a new twin still makes a new step set and a rebuild. `Submit` always takes the work and queues it by
  manager (`Work::owner`): each commit runs a manager's oldest update once the registration holds its steps (a step desc of the
  same or a newer serial). A plain update replaces the one queued before it when it has all of its steps; the noise slices are
  `sequential` and all run, one per commit. Work for a step whose twin died is dropped. The managers advance their state at
  Submit, so for the frames until a new step set is adopted, a reader may see a map whose work has not run yet.
- **No staleness timer.** Steps live until their twin dies or a new desc replaces them, so an unused step stays registered
  (never opened). Imports go when no step holds them and the last Gather did not read them.
- **`RenderGraph::Bindless` delegates to `DCLF::GpuTextures` for now.** The import machinery (import thread, relocations,
  streaming windows) stays in DCLF; moving it is a follow-up. `Null()` is fatal when it cannot be made. ImGui and
  ORGDeferredComposite moved too (seven users, not five). `ResolveBinding`'s `source_location` now names `Bindless.cpp`.

### Resize lead, phase 3: DCLF's main resources and native's engine size (decided by the agent, to confirm)

- **DCLF follows `DisplaySize` with a second main resources object, not with declared textures.** At the adoption point
  (`UpdateConditionsAtAdoption`), `Impl::FollowDisplaySize` reads the adopted `DisplaySize`. For a new size it makes the parts
  the display sizes (HZB and its counter, the foliage record, the two dispatch signatures) on the build executor. A later Begin
  copies every other part from the resources the frame runs (`CopySizeless`) and hands the result to the builds, the revision
  inputs and the ahead context (`AheadMain`). The frame keeps running the old resources until it installs the first scene whose
  payloads are for the new ones (`InstallScene` -> `AdoptPendingMain`). Why not `Frame::DeclareTexture2D`: the HZB's mip count
  changes with the size (a declared texture may only change its extent), and DCLF's builds declare one UAV per mip, so a size
  change needs DCLF builds anyway; DCLF's scene adoption is where its builds and recordings switch together.
- **Main targets, depth, refraction normals and the shadow mask are not made again.** They are the engine target set's slots
  (`ShareImage` finds them in `ImageRegistry`), so their new versions come with the target set's revision.
- **One new generation at a time.** Resources handed to the builds are adopted before newer ones are handed over, so no scene
  made for an older generation is left to adopt after a newer one. A window drag makes the sized parts for the newest size only.
- **For the frames between the target set's adoption and DCLF's, DCLF draws with the old HZB and foliage record.** The HZB pass
  clamps to its domain, so a larger window may cull at its right and bottom edges for those frames. The foliage record
  (`Policy::RecordFoliagePixels`, diagnostics only) is too small for a larger window then.
- **The pyramid's setup may run on the build executor** (`Api::DepthPyramid.h` says so now). GPU occlusion's setup only makes
  resources and loads a program.
- **Native: the set writes the swap chain's size into `BSGraphics::State` at a later set's start** (`Targets::BeginSet`).
  The engine's `CreateSwapChainViews` (SE `0x140d71d20`) reads its size from `GetBuffer(0)->GetDesc`, which in native mode is
  kFRAMEBUFFER's field handle and answers the adopted size. `SwapChainResizeBuffers` records the size each resize asks for
  (the window's client area for 0x0) once the set owns the back buffer (`Native::EngineSwapChainExtent`). The engine's
  `BSShaderRenderTargets::Create` reads `screenWidth/Height`, so it asks for its targets at the new size. Guest mode is
  unchanged: DXVK's back buffer answers its real size.

### Guest resize lead, phase 7: the Z-prepass in the resize frame (decided by the agent, to confirm)

Decision 94: a guest window resize stopped on a skeleton "host skipped" tap error.

- **Cause.** `CaptureDepthPass` compared the engine's kMAIN depth with the D3D11 object the last frame's main pass bound
  (`mainPassDepth`). The guest context's `OMGetRenderTargets` answers DXVK's object (the handle's `OMSetRenderTargets` unwraps),
  and at adoption the target set retargets kMAIN's field handle at the new version. So in the first frame at a new size the two
  objects differ, the depth pass logs "the Z-prepass stays with the main pass" once and skips its epoch, and the main opaque
  epoch still runs. The skeleton took RenderDepth's main depth clear for the Z-prepass host (`HostReady`), so main opaque's
  submission tripped `HostSkipped`. Native mode keeps handles in the context, so the compare passed there. DCLF's main
  resources hand-over (`76790607c`, `AdoptPendingMain`) is not involved: coverage and `RunEpoch` read both payloads against the
  same resources.
- **Fix (`423e04bc1`).** The main pass records whether it binds kMAIN (`mainPassBindsMainDepth`), checked against its own
  frame's version: the field handle itself, the same unwrapped object, or the same ORG resource (`GraphResourceOf`). The depth
  pass reads that answer. A slot keeps its field handle across a resize, so the answer does not change with the version, and
  the Z-prepass and main opaque epochs run together in the resize frame. The tap error and the host rule are unchanged.
- **Left as it was.** A main pass that really binds another depth still skips the Z-prepass with the warning, and the skeleton
  then reports `HostSkipped`, which names the wrong cause (A11). Making it fatal at the main pass, or telling the skeleton
  before RenderDepth's clear, is a follow-up.
- **Unchecked (not built).** `Tripwire::GraphResourceOf` on a native port's depth handle is assumed to answer the field's
  resource. If it does not, native frames skip their Z-prepass with the same warning: check native smoke for "the Z-prepass
  stays with the main pass".
- **Tests.** `guest+window-resize`, `guest+hdr-resize`, `guest+window-drag`, `guest+resize-drag` (no "host skipped", no
  "the Z-prepass stays with the main pass"), `guest` smoke, `dclf-toggles`, and `native-vulkan+window-resize` for the native
  compare.

### Native extent lead, phase 7: prepared passes read the bound extent (decided by the agent, to confirm)

- **Cause (decision 93).** `FrameAnchors::Texture::Width()/Height()` read the slot's current version, the adopted one. A
  revision is prepared on a lane while the old versions are still adopted, and ORG binds the versions the revision names
  (`VersionedResource::SelectVersion` through the host data's `IResourceVersions`). So the skeleton's kMAIN copy (decision 76)
  bound the 1280x720 versions and copied 1920x1080. With `42fbd617c` reverted, native never resized the engine, so nothing
  showed it.
- **Design.** `Texture::PreparedSize(const org::FramePreparationContext&)` answers the size of the version the preparation binds:
  `preparationData->Get<IResourceVersions>()->FindTexture(slot)`, else the slot's current version (the revision does not change
  that slot, and ORG binds the current one too). A plain resource answers its own size. A slot with no version, or a size of 0, is
  fatal (`PortFatal("frame anchors", ...)`). `Width()/Height()` stay for render-thread reads (commits), which run the adopted
  revision. A pass that wants a slot's size reads it in `Prepare`, not in `make`, since a staged build for a pending revision is
  made while the old versions are adopted.
- **Revision hashes drop the extent.** The copies' and the skeleton clears' `Revision` pushed the current extent. It follows the
  bound versions, which kept recordings check (`VersionsCurrent`), and a revision recording's ticket went stale at its own
  adoption when the hash read the adopted extent.
- **Converted callers.**
  - `AnchorPasses::Copy` and the depth copy (`CopyExtent`, both 0): the smaller of the two prepared sizes.
  - `AnchorPasses::Draw`: a viewport with width and height 0 is now the whole target (or mip) of the prepared version
    (`DrawViewport`, in a new `DrawFrame`). Before, a zero viewport drew nothing, so no caller used it.
  - The skeleton's clears (`ClearExtent`) for a slot target.
  - `ORGDeferredClears`: the GBuffer clear took its size at `make`; it reads the six targets' prepared sizes in `Prepare`
    (different sizes stay fatal).
  - Volumetric Lighting: the apply's viewport and the kMAIN to kMAIN_COPY depth copy took `Width()` at `make`; they use the zero
    forms above.
- **Checked, not changed.** ImGui's and Bink's commits (`heldTarget.Width()`, `target.resource.Width()`) and the skeleton's
  `RegisterCopy` size check run on the render thread against the adopted versions, which is what the frame binds. DCLF's direct
  sites (`TargetsOf`, `ShadowTargetsOf`, `ReflectionTargets`) take `Current()` on the render thread for views, not extents. HDR,
  the post chain, Sun Rays, Effects11, Upscaling and DCLF's epochs size from their keys' `DisplaySize` (the revision's inputs),
  so they agree with the engine slots only if the target set's versions and `DisplaySize` change in the same revision (the
  resize lead's handover). Skylighting's occlusion clear is a plain resource. CS-declared twins (`GraphResources`) still answer
  their adopted extent through `Desc()`; no `make` reads one for a size that follows the display.
- **Then** `42fbd617c` is applied again (`58f579e19`).
- **Unchecked (no build).** `org::FramePreparationContext` is forward-declared as a struct in `FrameAnchors.h` (it is a struct in
  `Render/PreparedPass.h`, which `FrameAnchors.cpp` now includes). `preparationData->Get<org::IResourceVersions>()` and
  `IResourceVersions::FindTexture` return a `TextureVersion` whose `resource` is the bound backing. `ORGDeferredClears`'s pass calls
  `RenderGraph::PortFatal(ORGDeferredClears::kName, ...)`, since `Port::Fatal` is protected. The non-graph stub of
  `PreparedSize` returns `{}`.
- **Risks.** A slot whose prepared size is 0 is fatal where it used to record an empty copy or clear. The skeleton's
  `RegisterCopy` still compares adopted sizes; if one side of a copy is a plain import made at the new size while the other's new
  version is pending, it would fail there. Decision 94's guest host-skipped stop is separate and still open.
- **Tests to run.** native-vulkan and native-d3d12: window-resize, resize, window-drag, resize-drag, hdr-resize. Guest: resize.
  Then the mode smoke set. Look for D3D12 command list refusals and Vulkan validation on the skeleton copies and the GBuffer clear
  in the resize frame, and for "has no version for the frame being prepared".

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
- **Submodules:** every pinned commit is on its remote. The branches were pushed on
  2026-11-20:
  - ORG (`extern/OpenRenderGraph`) at `bfd13bb`, branch `rev-step4`;
  - OMS (`extern/ORGModuleServices`) at `d48c924`, branch `rev-step5`;
  - `extern/BasicRHI` on `org-native/fg-dlssg-native`;
  - `extern/Streamline` on `org-native/fg-native-d3d12`;
  - `extern/dxvk` on `org-port/view-trace` and `org_integration` (`c1077d2b`).

  New submodule work goes on a branch inside the submodule, and is pushed before the
  superproject is pushed. Local submodule URLs were synced back to `.gitmodules` with
  `git submodule sync`.

  dxvk's nested `subprojects/dxbc-spirv` pins `10726e1` (optional shared memory zero-init),
  which is not upstream. It lives on the fork `panthuncia/dxbc-spirv`, branch
  `cs/optional-lds-zero-init`, and dxvk's `.gitmodules` points there (dxvk `c1077d2b`).
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
