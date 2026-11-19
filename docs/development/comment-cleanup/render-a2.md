# Comment cleanup: render-a2 (continues render-a)

## 1. Simplification or documentation targets
- Epochs/GraphConditions.h `GraphConditions::cpuPipeline` and Epochs/GraphConditions.cpp RefreshGraphConditions: the CPU-driven pipeline works two ways. In guest the DCLF epochs become external epochs that the sites execute in DXVK's command buffer. In native the graph submits them and the sites' late passes record inline. The 7-line comment was the only list of which passes stay (latched and index pool copies, texture transforms, object rows, tree LOD and grass single-phase culls, tree wind) and which go (builds, HZB, phase 2, readbacks, fade state). Suggest a "CPU pipeline: guest vs native" section in dclf-architecture.md, or retiring one of the two modes so the external-epoch list is not needed.
- Direct/ViewSites.h (file header, 13 lines) and Direct/ViewSites.cpp (file header, 13 lines): hosted views in the CPU pipeline. The reflection faces' and first-person depth's commits run inside the depth commit, are posted (HostedViewCommitted), attached when the depth site opens (AttachHostedViews, merging their shapes into the depth run's pre-work) and drawn after main depth. This is the CPU-side twin of render-a's "hosted epochs via callback" target. Suggest the same fix: pass hosted commits as data into the host, and document the flow once in dclf-architecture.md "Water reflections".
- Epochs/GraphBuilds.cpp (file header, 12 lines): the five-step order of a structural change (state changes, builds handed to the lane, frames keep old graphs and no snapshot is made, builds attach together at frame end, next frame adopts). Kept in an 8-line header. The rule is spread across GraphBuilds, BuildState, SnapshotBuilder and AdoptCurrentSnapshot. Suggest moving it to dclf-architecture.md "Revisions and recordings" and keeping one line here.
- Epochs/ScheduleExecutor.h ResolveSchedule: a 12-line doxygen listed about 20 install-time validation rules for FrameSchedule (hosted epochs, sharesWith, triggers, latch words). Cut to two lines. The rules are the schedule API's contract and belong in the Api/ schedule docs, not in the executor.
- Main/Resources.h SetClassPlan (`table`, `map`, PlainBucket lists): the colour segment's class sub-range layout and the packed map word layout ([set indices, forward, main, terrain, decal bucket counts, 0,0,0], then classOf, then per kind per set index...) are described only in comments, and must match GpuLayouts.h and BuildDrawsCS. Original map comment: "[set indices, forward buckets, main buckets, terrain buckets, decal buckets, 0, 0, 0], then IndirectState::classOf; with forward buckets, then each set index's forward bucket (kNoBucket: none) and each bucket's (staging first, capacity); with main buckets, then likewise each set index's main bucket and each bucket's (first, capacity); with terrain buckets, and then decal buckets, likewise." Suggest a struct or layout table shared with the shader header (GpuLayouts.h) so the layout is defined once.
- DrawFamilies/FamilySlots.h (file header, 10 lines): a draw family touches the pipeline lane, DrawFamilyLaneSlots.h, the main extension, MainOpaquePass, the schedule, the view epochs, MixViewShapesKey, PassCapture, DecideDrawFamilies and SceneBuffers::familyPipelines. The header was the only map of that. Suggest a single family registration point, or a short "Adding a draw family" doc.
- Epochs/MainEpochs.cpp (around `ready && impl->resources != resourcesBefore`, and `newResources`): new main resources mid-frame force a synchronous build on the render thread, the colour epoch is skipped for one frame, and the next frame waits for a matching snapshot. Explained by two 4-7 line comments. Same contract as render-a's AdoptCurrentSnapshot target; document together.
- Main/FrameLayout.h PreprocessStates: explicit DGC preprocess needs a never-submitted graphics list per site and run lease index, set up exactly as the execution will be. Cut to two lines. Worth a doc line in dclf-architecture.md next to the device-generated commands section.

## 2. Hazards kept
- Main/Resources.h, FrameLayout.h, DrawHelpers.h, BuildState.h, ViewSites.cpp, ShadowSite.cpp, SitePasses.cpp, Families.cpp: volk must come before every Vulkan header (include order).
- Epochs/MainEpochs.cpp: apply the engine's pending world state before the depth capture, else RSGetViewports can return the last shadow map's 4096x4096 viewport and the render area loses the device.
- Epochs/MainEpochs.cpp: Terrain Blending swaps the bound depth target during the depth pass, so use the engine's main depth. The field can be a tripwire handle.
- Epochs/MainEpochs.cpp: b12 is not rewritten through the hooked context every frame, b5 is written by CS's ConstantBuffer helper. Both are taken from CS's caches.
- Epochs/MainEpochs.cpp: an unsupplied slot read by a pipeline fails the constants check (VS b13), and a bound but unmirrored slot drops every object that reads it.
- Epochs/MainEpochs.cpp: the colour epoch replays the Z-prepass's vertex bytes so EQUAL passes. The forward epoch needs FrameParams.z raised after the composite.
- Main/FrameLayout.h: a command pool costs about 2 MB in the driver. D3D12 cannot hold several open lists on one allocator.
- Main/FrameLayout.h: kSortRankLimit, a draw range past 2^20 is a hard failure.
- Passes/PassHelpers.h FoldEyeIntoViewProj: the fold is inexact in float, a few 1/32 units off around 2^18, so a cull from it needs a margin.
- Passes/PassHelpers.h: CS_DCLF_ASYNC_COMPUTE=0 must be applied in AddPass, since ORG reads an external pass's queue from its description.
- Passes/Build.cpp: main plain buckets are written in place, so a scatter would copy stale staging over them.
- Passes/CorePasses.cpp: the sort scan runs every execution so counts stay zero. The diagnostic cuts keep an epoch's first pass because an empty epoch cannot be replayed (also ScheduleExecutor.cpp).
- Direct/Cull.cpp / CullSimd.h: CPU decisions must match BuildDrawsCS. Wide kernels must match the SSE pass bit for bit (operation order, no FMA). ParabolicSphereMask's sqrt, subtraction order and strict comparisons (FUN_14151a1e0).
- Direct/Cull.cpp: CullFrame readers count themselves before loading, seq_cst on both sides. AVX state checks (OS keeps YMM, ZMM and opmask state).
- Direct/Cull.cpp ApplyPlaneMargin: only the back hemisphere's plane moves, or a zero radius on the plane is rejected.
- Direct/Cull.h: the CPU sphere test is a subset of the GPU's AABB test, never draws what the GPU would not.
- Direct/ViewSites.cpp: reflection faces load the cube slice because the engine's sky is there. kPOST_ZPREPASS_COPY is FUN_1414cb6d0's depth target 7.
- Direct/ShadowSite.cpp: shadow slices are loaded because the engine's casters are already in them. An import's address can be reused, so views are keyed by the held backing.
- Epochs/BuildState.cpp: a build without a scope off the render thread would read state the render thread may be replacing.
- Epochs/GraphConditions.cpp: every attached conditions value is kept so handed-out references stay valid.
- Epochs/GraphBuilds.cpp: the build lane is never destroyed, a build may still be queued at teardown.
- Passes/Readbacks.cpp: counter readbacks held weakly so only the graph's pass keeps the host buffers alive.

## 3. Possible bugs or dead code noticed
- Direct/Cull.cpp (above `thread_local bool profilePortals`): an orphaned doxygen block for MainFrustumRejected sat above an unrelated variable. Dropped the orphan.
- Direct/Cull.h (above TakeCullProfileLine): an orphaned doxygen block for a ColourObjectWords-style function (the colour pass's words for the depth-culled batch) with no declaration. Dropped. The function may have been removed or moved.
- Passes/DrawHelpers.h (above PlainDrawArgumentsOffset): an orphaned doxygen block describing a bucket's plain indexed indirect draws sat above PlainDrawArgumentsOffset's own brief. Dropped the orphan.
- Passes/DrawHelpers.h: MainFramePushWords had a stray non-doxygen comment saying the frame slots' layout "is IndirectDraws' implementation" before its brief. Merged into one brief.

## 4. TODO/FIXME lines found
- None in this slice.

## 5. Coverage
Done (41 files, every file in the slice):
- Direct/: Cull.cpp, Cull.h, CullAvx2.cpp, CullAvx512.cpp, CullSimd.h, Families.cpp, ShadowSite.cpp, ShadowSite.h, SitePasses.cpp, SiteStats.cpp, SiteStats.h, ViewSites.cpp, ViewSites.h
- DrawFamilies/: FamilyDecisions.cpp, FamilySlots.h
- Epochs/: BuildState.cpp, BuildState.h, ExecutionOwners.cpp, ExecutionOwners.h, GraphBuilds.cpp, GraphConditions.cpp, GraphConditions.h, MainEpochs.cpp, ScheduleExecutor.cpp, ScheduleExecutor.h, ViewEpoch.cpp, ViewEpochRecord.cpp
- Main/: FrameLayout.h, Resources.h
- Passes/: Build.cpp, Common.cpp, CorePasses.cpp, DrawHelpers.h, Forward.cpp, IndexPoolPass.h, MainExtension.cpp, PassHelpers.h, PassesInternal.h, Readbacks.cpp, Shadow.cpp, VisibilityEvents.cpp

Not done: none.

Comment lines (full-line comments plus lines with a trailing comment), these 41 files: 1697 before, 1048 after. Most of what is left is the multi-line doxygen frame (`/**`, ` */`) around two-line briefs.

Every file was checked mechanically: with comments and whitespace removed it matches HEAD. No new double blank lines. Tools: C:\Users\matth\AppData\Local\Temp\claude\f5be53fdb6b18c23\c05c210a-3101-4ded-aeb9-5047fe31b158\scratchpad\comments\render-a2\ (cc.py, verify.py, one .spec per file).
