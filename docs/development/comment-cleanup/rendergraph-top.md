# Comment cleanup notes: rendergraph-top

The original text of every comment cut here is in the worktree's HEAD before the cleanup commit (`git show HEAD:<file>`).

## 1. Simplification or documentation targets

- `Frame/RecipeTable.h` `kStageTable`. Each CS stage's comment (5 to 21 lines) was the only record of the engine call order inside
  the stage, its commit point and why that point is safe (under-water ordering at FinishAccumulatingPostResolveDepth +0x51, the sun
  query before first-person geometry, Scaleform instance ranges 8-15 then 0-7, the three main-depth clears in a loading frame, the
  kMAIN copy on Solstheim). The file header said "Each CS stage's commit point and why it is safe are in the comment above it", so
  this was documentation by design. Cut to two lines each. Suggest moving the original per-stage text into
  `docs/development/org-native-frame-graph.md` ("The recipe") as a table per stage, with the table pointing there.
- `FrameAnchors.h` `ClientDesc` and `PassDesc`. These were the API reference for the client contract (registerResources, commit,
  active, sharedConstants, gates, executed, declared, instances, rows, repeats, segmentOnly, instanceMajor, matchesById; and order,
  make, once, atExecution, gpuGated, row). Twenty-plus fields with subtle interactions; the field list is not explained anywhere
  else. Suggest a "Writing a frame anchor client" doc page, and fewer flags (several only matter to one client: matchesById and
  instanceMajor to Scaleform, repeats to ImageSpace).
- `FrameAnchors.h` `Scheduled` / `InStage` / `HeldNow` / `CanTap` / `TapMayLag` / `StageInputs` / `StagePlace`. Seven overlapping
  "may I draw now" queries, each with a paragraph explaining how it differs. Suggest one query returning a small state struct.
- `RenderGraphRuntime.h` `SignalFramePace` / `WaitFramePace`. Original: DXVK's own frame-latency wait is off for the frame
  generation presenters, so the host's slot rings were the only bound, and their waits landed mid-frame and released the render
  thread in bursts (frames alternating ~4.5 and ~17.5 ms). Worth a doc note.
- `BufferRefs.h` `RefLatch` / `DeclaredRefs` / `CheckDeclaredRefs`. Self-described interim form: refs latched at commit and
  checked against declarations in Prepare, with frame-keyed history to avoid false mismatches. Target: write refs in Prepare from
  the declaration and drop the latch, as the comment planned.
- `ImGuiBasicRHI` atlas pages: ImGui textures as versions of four fixed versioned pages, with whole-texture restaging until an
  upload of the bound version is recorded. The header comment (34 lines) was the only explanation.

- `FrameAnchors.cpp`, the CS stage model (`kDeclared` block, `TapDeclared`, `SubmitSegment`, `OpenInputs`). A stage is one epoch
  whose taps only stage values into a staging run and whose last tap commits and submits one ticket. The comments at each site
  re-explained taps, staging runs, frame slots, published copies and tickets. Suggest one section in
  `docs/development/org-native-frame-graph.md` ("The recipe") as the single reference, and fewer kinds of state per stage.
- `FrameAnchors.cpp` `Schedule`, `RequestDeclared`, `SegmentRecordings`, the staged-build switch and the "starvation rule".
  Per segment and frame slot there is a schedule, a set of kept recordings, a staged build, a frame description and a "wanted"
  description, and the stage may lag. The comments were the only description of how a staged build switches in, why recordings of
  a staged build must not be evicted (alternating TAA history rows starved it) and how the slot keeps an old schedule while a new
  version records. Candidate for simplification: one versioned "schedule" object per segment with explicit states instead of six
  parallel arrays in `State`.
- `FrameAnchors.cpp` `UpdateKey` / `BuildKey` (absent passes). The rules for when a key asks for a build (new pass wanted, load,
  kind switch, provisional key, kKeyHold) were only in an eight-line comment. Suggest documenting in the "Absent passes" doc
  section and reducing the triggers.
- `FrameAnchors.cpp` `GatherDeclared` and `GpuGates`. Whether a pass records ahead, at execution (late batch), inline, or keyed by
  its gate answers (`CS_ORG_KEEP_GATED`) depends on four flags and the mode. The comment was the only overview.
- `FrameAnchors.cpp` stage windows (`WindowOpen`, `TapDeclared` ending earlier stages, `commitBefore`). A stage whose commit point
  does not come (deferred+water when DCLF's forward epoch does not run) is ended by a later stage's first tap or by the next GPU
  work in `commitBefore`. Unexpected cross-stage interaction; worth a doc diagram.
- `FrameAnchors.cpp` `EngineImport` and `Import`/`ShareImage`: id imports versus loose slots, who owns what, and the "one image,
  one graph resource" rule spread over MadeTexture, FindImage, EnsureViews, Engine::Targets and native textures. Five lookup
  paths before an import is made. Suggest one image-to-resource registry.
- `FrameAnchors.cpp` `RetiredGates`. Old graphs still hold a departed client's passes and replay them, so every run must close
  the departed client's gates. Unexpected; a design where a rebuild is required before a client leaves would remove it.
- `RenderGraphRuntime.cpp` queue ordering (`computeQueue`, `copyQueue`, `uploadQueue`, `streamPoint`, `computeExit`, the
  stream-order guard, `ComputeSubmitter`, `WaitForPoints`). About forty lines of comments were the only explanation of how off-stream
  queues are ordered against DXVK's stream, why every queue takes DXVK's lock, why shared copy queue batches wait on the CPU and
  why exit waits use their own semaphores. Original key facts: concurrent compute and DXVK submissions lost the device (page fault
  at address 0, 0 of 39 after serializing); a graph copy ahead of the frame values' signal deadlocked the GPU (2026-11-01); DXVK's
  own transfer queue is blocked by `setSubmissionBarrier`; `vkDestroySemaphore` needs every batch referring to it complete, and
  rebuilds destroy graph timelines. Belongs in a doc page (render-graph.md) with a diagram.
- `RenderGraphRuntime.cpp` the CS_ORG_* host switches in `Initialize` (KEEP_LIVE, COALESCE_SUBMITS, MERGE_HANDOFFS,
  UPLOADS_WITH_BATCHES, KEEP_GATED, SUBMIT_THREAD, MERGE_TICKETS, QUEUE_PRESENT, QUEUE_UPLOADS, SCOPED_FRAME_WAIT, CLOSED,
  EXTERNAL_HOME, RESTING_LAYOUTS). Each had a paragraph. Thirteen default-on toggles are a test matrix nobody runs; consider
  retiring the ones that are always on.
- `RenderGraphRuntime.cpp` `CS_ORG_QUEUE_UPLOADS`: a pass must declare every upload it reads, which is why DCLF's per-frame
  publications are late-bound VersionedBuffers. Original: "Without that, the Z-prepass epoch's compute batch read the portal
  programs before their upload landed and the interpreter looped on a garbage record count until Windows reset the GPU (RX 6600,
  camera sweeps, 2026-11-12)."
- `RenderGraphRuntime.cpp` shader hot reload safe point (`cs.org.pipeline-registry` extension, `ServicePipelineReload`): only an
  owner-thread build publishes staged pipelines. Document in dclf-hot-reload.md.

## 2. Hazards kept

- FrameAnchors.cpp: D3D11 texture releases must run on the render thread (RetiredTextures, ~EngineImport).
- FrameAnchors.cpp: Guard is recursive since run callbacks may register; never take Renderer::Lock under it.
- FrameAnchors.cpp: a loading screen presents from a job thread (SE 38085); one-shot passes wait for a main-thread frame.
- FrameAnchors.cpp: one image must have one graph resource, or two trackers race on layout moves.
- FrameAnchors.cpp: drain the old owner's submissions before taking the anchors' lock.
- FrameAnchors.cpp: a recording failure stops the game in every build; a busy published copy is a pacing defect, fatal.
- FrameAnchors.cpp: an epoch must not be the last GPU work before a third-party Vulkan consumer (lateHook).
- FrameAnchors.cpp: recordings of a staged build must not be evicted or it never switches in.
- RenderGraphRuntime.cpp: VK_EXT_descriptor_heap rendered DLSS black on NVIDIA; vulkanMemoryModelDeviceScope needed for DXC atomics.
- RenderGraphRuntime.cpp: separateDepthStencilLayouts for FidelityFX on AMD (D24S8 backed by D32S8).
- RenderGraphRuntime.cpp: binding model callback runs inside DXVK device creation, must not call into DXVK or throw.
- RenderGraphRuntime.cpp: every queue of DXVK's device is used under DXVK's submission lock.
- RenderGraphRuntime.cpp: shared copy queue batches wait for their points on the CPU first.
- RenderGraphRuntime.cpp: stream exit waits use computeExit/copyExit, never graph timelines.
- RenderGraphRuntime.cpp: stream-order guard flushes before a wait that would wait on the current list.
- RenderGraphRuntime.cpp: EmitCommands wrappers are released at frame end, outside stage windows.
- RenderGraphRuntime.cpp: resting layouts keep compression on AMD's Windows driver.
- RenderGraphRuntime.cpp: DXC must load from beside the DXVK DLLs (another dxcompiler.dll lacks SPIR-V).
- RenderGraphRuntime.cpp: shutdown leaks the graph if DXVK dropped queued submissions, and at static destruction.
- RenderGraphRuntime.cpp: frame-pace wait is bounded so a lost signal cannot hang the game.

- RenderGraphRuntime.h: the frame-wait timeline's signal must be submitted before Present enters DXVK, or AMD's windowed present
  deadlocks on DXVK's submission lock.
- RenderGraphRuntime.h: WaitBeforePresent, since a present can block on the frame's GPU work while holding the submission lock.
- RenderGraphRuntime.h: BuildIfRequested, not the host's own, so a failed host thread stops the graph instead of throwing.
- FrameAnchors.h: DecalQueries is not EngineQueries, because an inactive client's query reset and resolve would overwrite unread counts.
- FrameAnchors.h: once passes must be safe to run twice.
- GraphResources.h / Frame/FrameDriver.h: new slot versions only at a frame boundary (RequestSize, AtNextBegin).
- DxvkImageImport.h: one graph resource per image, or layout moves race (duplicate-image check).
- Frame/RecipeTable.h: the sun query must reach the GPU before first-person geometry; the pre-depth clear before the Z-prepass draws.
- RasterProgram.h: ProgramCache sets stage defines on the requesting thread, since CS's common defines read State.

Dropped from the feature table comment (kept here): maintenance5 is optional because AMD's legacy Polaris driver lacks it
(BasicRHI then uses VkChainPipelineCreateFlags); device-generated commands are optional, DCLF stays off without them.

## 3. Possible bugs or dead code noticed

- FrameAnchors.cpp: before `windowsClosedHook`, a brief "Whether any stage holds staged values not committed yet" sat on the
  wrong declaration (it belonged to `UpdateStagesOpen`); both briefs removed.
- FrameAnchors.cpp: an orphan doc block about "the anchors' passes in each graph" sat above `SegmentHolds`; and an orphan brief
  for a "segment that holds a shared anchor's named clients now" (`OpenStepAt`?) sat above `WindowOpen`. Removed.
- RenderGraphRuntime.cpp: orphan "BasicRHI hands every queue submission here" above `ContributorEpochId`; and the late-consumer
  hazard comment above `segment = a_segment;` in ExecuteEpoch was misplaced. Removed (the hazard is kept at FrameAnchors' lateHook).

- Frame/FrameDriver.h: two briefs were stacked above `NoteKeyBuild`, the first belonging to `NoteBuildSwitch`. Replaced with
  NoteKeyBuild's brief; NoteBuildSwitch now has none.
- DxvkImageImport.h: an orphan doc block for a buffer-ref import function (a_buffer, a_keep, a_address) sat above
  `NoteImageResource`, merged with its doc. The function it described is not declared there. Removed.
- DxvkOrgInterop.h (not edited, see coverage): the comment "Describes a buffer, texture or SRV and marks it stable" sits above
  `PFN_dxvkEnqueueQueueCallback`, and "Copies the last completed snapshot..." sits above `PFN_dxvkSetResourcesSpillable`. Both
  look displaced from the declarations they describe (in the DXVK mirror too, presumably).
- RenderGraphRuntime.h: the old file header described each epoch flushing DXVK and submitting under its lock with full barriers;
  that no longer matched the in-stream design. Rewritten.

## 4. TODO/FIXME lines found

None in the slice.

## 5. Coverage

Done (17 files): FrameAnchors.cpp, RenderGraphRuntime.cpp, FrameAnchors.h, RenderGraphRuntime.h, GraphResources.h,
Frame/RecipeTable.h, BuildManager.h, ImGuiBasicRHI.cpp, ImGuiBasicRHI.h, BufferRefs.h, PublishedResources.h, AnchorPasses.h,
Frame/FrameDriver.h, DxvkImageImport.h, RasterProgram.h, UndeclaredAccess.h, Frame/Recipe.h.

Deliberately not edited: DxvkOrgInterop.h. It mirrors `extern/dxvk/src/d3d11/d3d11_org_interop.h` ("keep the two identical"),
so its comments should change only together with the DXVK fork's copy.

Not started (smaller files, about 1,600 comment-bearing lines in all): Frame/PostChain.cpp, GraphResources.cpp, RunGate.h,
BuildManager.cpp, Frame/FrameDriver.cpp, ProgramCache.h, Frame/FrameDescription.h, ShaderBindings.h, SharedConstants.h,
Frame/Transients.h, FrameStaging.h, GpuTimingSource.h, PublishedResources.cpp, ComputeProgram.h, Frame/PostChain.h,
AnchorPasses.cpp, DxvkImageImport.cpp, Frame/Recipe.cpp, RasterProgram.cpp, ExternalWork.h, GenerateMips.h,
UndeclaredAccess.cpp, RunShapes.h, Frame/FrameResources.h, CensusWindow.h, FrameSequence.h, Switches.h, Backend.h, PrefixSum.h,
GpuTimingSource.cpp, ProgramBuild.h, Frame/EngineTargetUse.h, BufferRefs.cpp, MemoryWindow.h, UavClearPass.h,
SharedConstants.cpp, ShaderTarget.h, ComputeProgram.cpp, ScheduleWindow.h, FrameAnchorSelfTest.cpp, GpuLoadPass.h,
Frame/SegmentId.h, GraphResourcesSelfTest.cpp, FrameAnchorSelfTest.h, MemoryWindow.cpp, and the files with under 10.

Numbers (lines carrying a comment, trailing comments included): whole slice 5,086 before, 3,500 after. The 17 done files:
3,361 before, 1,775 after. `git diff -w --stat` over the slice: 17 files, 1,006 insertions, 2,592 deletions. Every file was
checked mechanically: with comments and whitespace stripped, each is identical to HEAD.
