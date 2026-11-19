# Comment cleanup: render-a

## 1. Simplification or documentation targets
- IndirectDraws.h AdoptCurrentSnapshot: the comment was the only statement of a cross-thread contract. The frame blocks for the builder, which is only safe because nothing the builder needs is posted later in the frame (revision inputs at the build point, main-resource context at Setup via PostResourcesAhead, publication gate's scene-pass half open, owner-thread requests served inside the wait). Suggest: make the wait's inputs explicit (one "frame-start inputs" object posted before the wait) or document the contract in dclf-architecture.md.
- IndirectDraws.h CaptureDepthPass / SubmitViewEpoch: hosted epochs (reflection faces, first-person depth) are committed into the depth epoch's commit through a std::function callback, and SubmitViewEpoch returns NoRevision if called anywhere else. A re-entrant call path that only works from inside another commit. Suggest: have the host take a list of hosted commits as data instead of a callback, or document it.
- IndirectDraws.h ViewLodRootsOwned / ResolveViewFaces: 8-line comments listing which LOD roots a cube camera update skips (tree root while faces' tree LOD is DCLF's, LOD land/objects while Lod gates register a show veto and both classes are on, none while residueDue). The rule spans Reflection, Lod and DCLF. Suggest a single table-driven "who owns which view root" function, documented in dclf-architecture.md.
- IndirectDraws.h WriteAccumulated: DCLF reproduces the engine's kAccumulated cull side effect (stood-in roots, wide writes of listed roots, nodes between, cell roots) for observers like Fades and TreeAnimation. Only explained by the comment and skyrim-engine-notes.md. Keep it in that doc.
- FrameDriver.cpp DepthCommitted (view work at the depth commit): the depth epoch hosts the reflection faces and first-person depth so they draw after its index pool copies. If they ran against the previous frame's pool, a reused slot reads old indices with new vertices and faults the device. The ordering lives only in this comment and in Schedule/Frame.cpp. Suggest: make "draws after the index pool copy" an explicit graph dependency so hosting is not needed for correctness, or document it in dclf-architecture.md "Water reflections".
- FrameDriver.cpp OcclusionReady / OcclusionNeedsEngine and LoadHold: occlusion maps have three owners per frame (DCLF's shadow epoch, the engine at its occlusion point, or held without the engine's cull) and a re-entry streak counter (kOcclusionReentryFrames, occlusionEverDrawn). The rules were spread over 5-line comments. Suggest a small state machine type per map with named states, or a doc section.
- LoadHold.h (file header): the hold keeps the load coordinator's UI message (+0x211), holds presents when there is no menu, doubles as the re-entry hold after coc/map, withholds passes and skips engine culls, and ends on a five-part settle rule. A lot of engine coupling explained only here. Suggest moving the full rule list into dclf-architecture.md "Load screens" (original header was 19 lines).
- Impl/SnapshotBuilder.h publication gate (snapshotTakenFrame, publicationGated, frameWaiting, PublicationGateClosed): a hand-rolled lock-free gate between the frame, the coordinator and the builder with seq_cst pairing, plus a frame-waiting override to avoid a deadlock (a payload held for a scene pass that only wakes after frame start). Hard to verify from code. Suggest replacing with a single mutex+condvar state object, or a graph primitive (ReadyGate-style) per the project's "graph primitives over bookkeeping" direction.
- Direct/SiteFrame.h SiteOut / BindHeaps: recording into DXVK's command buffer from a callback needs every state bind in secondaries, heaps re-bound before each execution (NVIDIA MMU faults otherwise), and DXVK and the graph must negotiate one descriptor model. Hazard kept short in code; worth a doc section in dclf-architecture.md "The CPU-driven pipeline".
- Direct/SiteFrame.h State::siteRuns / pending / handedOff and RecordExecutingSite: three ways a late pass finds its run (guest executing pointer, native lease-indexed map, GPU-driven hand-off), because the late pass may run on the submission thread after the render thread opened the next site. Suggest one mechanism: always pass the run through the epoch's run lease.
- Direct/Direct.h Experiment bits: 11 A/B switches and 5 diagnostic bits kept permanently in the hot path. Suggest retiring the switches whose experiment concluded.

- Epochs/ViewEpoch.h PerFrameBlock: rebuilding the engine's VS_PerFrame (b12) by hand, register by register (c16, c20/c24 swapped against FrameBuffer.hlsli's names, c28, c32, c36, c41, c43/c44, fDRClampOffset). Correctness depends on matching SetCameraData exactly. Suggest a struct mirroring the block layout with named fields, or a doc table in skyrim-engine-notes.md. Original comment:
  "VS_PerFrame (b12) for a camera, ShadowCameras::kBlockBytes at a_out, as the engine's SetCameraData builds it: the frame's registers (a_frame, the renderer's last upload) with the camera's over them - CameraView, CameraProj, CameraViewProj and CameraViewProjUnjittered; c20 the projection's inverse and c24 the projection (the engine's order: FrameBuffer.hlsli names them the other way), c28 the view's inverse, c32 the view-projection's, c36 the projection's; CameraPosAdjust. a_jitter: the TAA / upscaler jitter added to the projection's third row (State::projectionPosScaleX/Y). a_previous: CameraPreviousViewProjUnjittered (c16) and CameraPreviousPosAdjust (c41); none, both zero (with fDRClampOffset in c41's w) and the dynamic resolution registers (c43, c44) from the renderer state, as for a reflection face."
- Epochs/ViewEpoch.h ViewResources / OwnsFrameConstants: a view without a depth pass (the reflection faces) owns a full copy of the main frame constants, frame block views, per-face block views and copies of the main frame buffers, all refilled from CPU mirrors at its commit. "Owns frame constants" is inferred from "has no depth pass". Suggest an explicit trait instead of the inference, and a doc section on view frame constants.
- Sites.h (file header, 23 lines): guest vs native recording of the CPU pipeline's sites and the frame-slot lifetime rule. Cut to 9 lines. The lifetime rule (everything a site makes lives until the CPU-frame timeline signal; latch regions in an external run retiring on the same signal) belongs in dclf-architecture.md "The CPU-driven pipeline".

## 2. Hazards kept
- IndirectDraws.h / FrameDriver.cpp: AdoptCurrentSnapshot's wait must not depend on anything posted later in the frame. Five seconds is a fatal defect.
- IndirectDraws.h NoteBlendedFrameParams / Impl.h blendedFrameParamsZ: engine raises FrameParams.z after the colour commit. It gates Lighting's forward fog.
- Impl.h prepassVS / mainMinDepth: engine rewrites per-frame constants between depth pass and composite, so colour reuses Z-prepass bytes for EQUAL.
- Impl.h free payload: an idle payload must hold nothing or it pins the retirement chain.
- FrameDriver.cpp: "GPU completion timeline failed" means a fence read UINT64_MAX (removed device). No fatal message box, it hides behind the full-screen window.
- FrameDriver.cpp BeforeOpaquePass: engine binds/clears G-buffer only when a draw applies state. Clouds rebind Cloud Shadows' t26 by the end of the pass.
- FrameDriver.cpp: depth-hosted view epochs must draw after the index pool copies or the device faults.
- FrameDriver.cpp / Sites.cpp: the CPU frame signal must be queued before Present.
- FrameDriver.cpp LatchAccumulator: the only point where currentAccumulator is the main camera's.
- LoadHold.cpp/.h: engine addresses SE 13214 / AE 13363 +0x11A, +0x211, Mist Menu +0x2E5 and +0x3C0.
- LoadHold.cpp: a cold shader cache stalls counts for seconds, so finished builds count as progress.
- Impl/SnapshotBuilder.h: tables compared by value since the coordinator pools them. Catalog held weakly so a freed address is not mistaken for a new one.
- Impl/SnapshotBuilder.h publicationGated: seq_cst pairing so no scene-pass wake is missed.
- Direct/Sites.cpp PoolFirstsOf: a reassigned index-pool range read with an old sequence faults past the vertex buffer.
- Direct/Sites.cpp: D3D12 refraction must use heap indices, device addresses fault on AMD. D3D12 refuses UAV on upload-heap buffers.
- Direct/Sites.cpp: compute-wait must be emitted right after the segments, or the GPU waits for itself.
- Direct/Sites.cpp ReleasedFrame: BasicRHI destroys take device locks, so they leave the render thread.
- Direct/Sites.cpp ThreadBusy: thread search costs tens of ms, never on the render thread.
- Direct/Direct.h: pushed pipelines are Vulkan only. Pulled colour costs 25-30 registers on NVIDIA.
- Direct/SiteFrame.h: views of a released image drew into memory DXVK had reused. BindHeaps before every execution (NVIDIA MMU faults). DXVK and graph must share one descriptor model.
- Direct/SiteFrame.h: late pass may run on the submission thread after the next site opened.
- Direct/Recorder.h: D3D12's SV_InstanceID does not count the first instance, so its signature writes the instance word first.
- Direct/Recorder.h / Recorder.cpp: first person's two-target pass and the reflection's R16G16B16A16 cube must keep view pipelines (VUID-vkCmdDrawIndexed-colorAttachmentCount-06179, -dynamicRenderingUnusedAttachments-08911).
- Direct/Recorder.cpp: the indirect draw buffer is write-combined, written in order and never read. A D3D12 run leaves its last instance in the instance word.
- Direct/Recorder.cpp RecordFamilyDraws: families leave their own pipeline and push data bound, so BeginList must run again.
- Direct/Sites.h SubmitQueued: anything waiting on unflushed DXVK work would wait for the waiting frame itself.
- Epochs/ViewEpoch.h: kViewBlockBytes must stay 256-aligned for the descriptor-buffer model. First person's depth is kPOST_ZPREPASS_COPY, bound by FUN_1414cb6d0. View passes are redeclared only at the view's import, never mid-frame. The scene lane must never read ViewEpochState::targets.

## 3. Possible bugs or dead code noticed
- Direct/Sites.h: ExecuteEpoch's doxygen sat above AdoptSiteRun, with ExecuteEpoch declared after it. The block now keeps both briefs together above AdoptSiteRun, in the original order. Worth moving ExecuteEpoch's brief next to its declaration (a code reorder, not done).
- Direct/Sites.cpp (around FinishCulled): a doxygen block for a pool cull job (CullJob) sat directly above FinishCulled's own brief, so the job function's brief was orphaned. Dropped the orphan.
- Direct/Direct.h Experiment: the bit values are declared out of order (kExpPortalBatch = 1 << 2 after kExpPrefillLive = 1 << 4). Harmless but easy to misread when adding a bit.

## 4. TODO/FIXME lines found
- None in the files done.

## 5. Coverage
Done (19 files): IndirectDraws.h, Impl.h, FrameDriver.cpp, FrameDriver.h, LoadHold.cpp, LoadHold.h, IndirectDrawStats.h, Internal.h, Impl/SnapshotBuilder.h, Impl/Producer.h, Impl/Views.h, Impl/Stats.h, Direct/Sites.cpp, Direct/Direct.h, Direct/SiteFrame.h, Direct/Sites.h, Direct/Recorder.h, Direct/Recorder.cpp, Epochs/ViewEpoch.h.

Not done (41 files), untouched:
- Direct/: Cull.cpp, Cull.h, ViewSites.cpp, ViewSites.h, ShadowSite.cpp, ShadowSite.h, SitePasses.cpp, CullSimd.h, Families.cpp, SiteStats.h, SiteStats.cpp, CullAvx2.cpp, CullAvx512.cpp
- DrawFamilies/: FamilySlots.h, FamilyDecisions.cpp
- Epochs/: everything except ViewEpoch.h (MainEpochs.cpp, ScheduleExecutor.cpp/.h, ViewEpochRecord.cpp, ViewEpoch.cpp, GraphConditions.h/.cpp, BuildState.h/.cpp, GraphBuilds.cpp, ExecutionOwners.h/.cpp)
- Main/: Resources.h, FrameLayout.h
- Passes/: all (DrawHelpers.h, PassHelpers.h, Build.cpp, CorePasses.cpp, Common.cpp, Readbacks.cpp, Forward.cpp, VisibilityEvents.cpp, MainExtension.cpp, IndexPoolPass.h, Shadow.cpp, PassesInternal.h)

Comment lines (full-line comments plus lines with a trailing comment):
- Files done: about 2261 before, 1311 after.
- Whole slice: 3958 before, 3008 after (the not-done files still hold about 1697).

Every done file was checked mechanically: with comments and whitespace removed it matches HEAD. Doxygen continuation lines keep the ` * ` style.
