# Comment cleanup notes: gpu-engine-handoff

Slice: `src/Features/DrawcallLimitFix/Core/Gpu/`, `Core/Engine/`, `Core/Handoff/` in the test-runner worktree.

## 1. Simplification or documentation targets

Ordered by how much the comments were carrying.

1. **Gpu/FrameValues.h (class doc) and FrameValues.cpp (Impl: KeptMover/KeptRoot, ApplyPlans, MarkDue, settling, owedKeys, RefusedShading, CheckEveryMover).**
   This is the moved-only sampling. Placement plans arrive as deltas onto a kept set. A move-key index maps engine writer events to movers and roots. A mover stays due until a sample changes nothing. Refused leases carry items over, a shading slot is named only once, and the registered pools run after the built-in sets at every step. The 32-line class doc was the only full account, and from the code you can only rebuild it by reading roughly 3,000 lines.
   Suggestion: give the "FrameValues" section of dclf-architecture.md the state machine for a kept item (joined, due, settling, owed, left). Better still, split Impl into three parts: a kept-set or plan applier, a due-set tracker, and the uploader. The original class doc is quoted at the end of this file.

2. **Gpu/FrameValues.cpp WaitSignalSubmitted / BeforePresent.**
   This is a three-party deadlock. A present can block on the frame's GPU work while it holds the submission lock that the upload queue needs, and that work waits for the copy-queue signal. There are three more cases: native mode's queued present, the rule that the copy batch waits only on sealed points, and the need to flush DXVK's stream before waiting (copy-queue-sealed-stall). Original: "The frame's signal on the copy queue must be submitted before the present. A present can block until the frame's GPU work is done while it holds the submission lock the upload queue takes (DXVK presents under it; BasicRHI's Vulkan present holds its timeline lock; AMD's windowed present blocks), and that work waits for the signal: a deadlock."
   Suggestion: a doc page on copy-queue signal and present ordering, with one diagram per backend. Simplification: make the frame-wait signal part of the graph's own submission, so present ordering cannot invert.

3. **Handoff/PrimaryCull.h (class doc) and PrimaryCull.cpp (Cut, PlanEntry, RunAdmission, StandIn, SyncRootOwnership).**
   This is the stand-in for the main camera's list jobs. An entry is eligible only if every OnVisible under it is one DCLF stands for. Admission is by readiness, and entries can be walked, whole or mixed. Pool plans carry tickets. Root ownership states feed the TreeWind and Fades observers, and kAccumulated writes are needed or FUN_1402cff60 stops animating stood-in roots. The class also holds all the scene-list filter state.
   Suggestion: move the scene-list filter (SceneLists.cpp and its roughly 40 members) into its own class. Write a "main camera stand-in" doc section with an entry's lifecycle (candidate, planned, eligible, admitted, plus the walk and mixed flags).

4. **Handoff/SceneLists.cpp and PrimaryCull::PublishListFilter.**
   There are three list modes (Engine, Rebuild, Keep). Filter versions are deltas, and structure events are replayed onto kept lists. There are also the category-node-6 skip and the hidden-entry rules. The BuildSceneLists, ClearList and CullList detours must go in together, or kept lists grow every frame. This is very subtle interplay with engine jobs.
   Suggestion: measure Rebuild alone. If it is cheap enough, drop Keep mode with its delta and event replay. Otherwise document it in dclf-architecture, "The scene lists without DCLF's roots".

5. **Handoff/PassCapture.h/.cpp.**
   Passes are withheld at RegisterPass and at six direct group or list insertion hooks (FUN_1414b2330, FUN_1414b2a60, FUN_1414b2c20). Thread-local registration context (t_shadowRegistration, t_registrationAccumulator, t_focusAccumulationCovered) decides which view a pass belongs to. Group renderers can be created mid-registration, and there are per-view gates and the load hold. Which hook sees which accumulation hint was documented only in comments.
   Suggestion: a table in the engine notes (hint, group, insertion site, DCLF hook). Simplification: one withholding entry point that every insertion hook calls with (batch, pass, site).

6. **Gpu/Pipelines/DrawPipelines.h layout constants** (push words, row headers, t116-t127 registers, RecordBackingIndex, shadow push words).
   This is the binding contract with DCLFObjects.hlsli, DCLFFrameRecord.hlsli, Lighting.hlsl and Utility.hlsl. Only comments on both sides and a few static_asserts hold it together.
   Suggestion: generate one shared header that both C++ and HLSL include, or write a binding-layout doc page.

7. **Gpu/Pipelines/DrawPipelinesImpl.h SetVersion, Versions and state classes.**
   Execution-set versions are written only when unreferenced (VUID 11071, 11029). State classes exist because a set's pipelines must share fixed-function state (VUID 11040). Free-version reuse relies on acquire fences.
   Suggestion: a doc on DGC execution-set versioning.

8. **Gpu/Pipelines/DrawPipelines.cpp Impl::Initialize.**
   The device's features choose one of three draw paths (DGC, counted plain draws, CPU-forced). The push-data size has to fit maxPushDataSize and the descriptor-buffer spill (128 vs 256 B), and D3D12 uses root constants only.
   Suggestion: one table of which GPU class gets which path, in the docs.

9. **Gpu/Pipelines/MainPipelines.cpp BuildMain.**
   These are the engine's depth and blend rules for decal groups 1-3, terrain groups 1-2, forward keys, LOD write mode 1, and EQUAL vs LESS_EQUAL against DCLF's prepass. The engine emulation is spread across inline comments.
   Suggestion: a data-driven rule table, plus a table in the engine notes ("Decals", "Terrain").

10. **Gpu/GpuTextures.cpp Registry and Gpu/GpuResources.**
    Pinless relocations race with in-flight imports (tickets, kUnclaimedFor, importsInFlight, Supersede, Register). The same pattern is written twice, once for textures and once for buffers.
    Suggestion: one shared relocation helper. Or have DXVK report relocations per import handle, which would remove the ticket race.

11. **Gpu/PlacementRows/BillboardRow.h.**
    The engine's billboard turn (NiBillboardNode::OnVisible 0x140d3f720, face modes 0x140d3e650) and the row layout that Effect.hlsl DCLFBillboardWorld decodes. It was a 25-line comment.
    Suggestion: move the engine part to the engine notes, "Billboards", and the layout to the shared header from item 6.

12. **Handoff/ShadowViews.cpp PredictTargets and the ShadowReject enum.**
    These predict how RenderShadowmap assigns targets and slices, including the quirk where hemisphere 1 draws into hemisphere 0's slice. They also hold the decal, volumetric and point-light caster rules (FUN_1414b2a60, plus DecalPointLight, which was never observed).
    Suggestion: engine notes, "shadow maps".

13. **Gpu/Pipelines/ShaderPrograms.cpp VertexAttributesOf.**
    The attribute list is kept by hand to match DCLFPulledInput in Lighting.hlsl and Effect.hlsl.
    Suggestion: reflect the stage's inputs instead of mirroring the defines.

14. **Engine/Capture/SceneEvents.h (MoveEvents, SwitchEvents docs) and SceneEvents.cpp.**
    Each engine writer hook carried a multi-line description of the engine function and its callers. They are shortened to one or two lines with the address kept.
    Suggestion: an engine-notes table of writer hooks (function, address, fields written, event pushed).

## 2. Hazards kept (index)

- Gpu/FrameValues.cpp: hint-9 fade groups draw last child first (FUN_1414b2330). Sorting by each part's own bound drew a dark mask over hair.
- Gpu/FrameValues.cpp: the NiSkinInstance+0x60 critical section, SE/AE only. RaceMenu swaps skinPartition with no engine writer.
- Gpu/FrameValues.cpp: the copy-queue signal must be submitted before Present. Flush DXVK's stream before waiting.
- Gpu/FrameValues.cpp: palette blocks are reused across actors. Settling items are validated against the applied plans.
- Gpu/FrameValues.cpp: producers run strictly in order with monotonic signals. Engine refs are released only on the render thread.
- Gpu/GpuTextures.h: a 2D descriptor read as cube/3D/array loses the AMD device. A null texture read as a buffer is not zero on RDNA.
- Gpu/GpuTextures.cpp, GpuResources.cpp: unwrap tripwire handles before DXVK sees them. ORG-owned engine buffers are never passed to DXVK.
- Gpu/GpuTextures.cpp: imports are legal only inside an epoch, so a refusal outside one is not cached.
- Gpu/GpuTextures.cpp: the engine sampler table is [6][5] on AE 1.6.1170 and SE 1.5.97.
- Gpu/PlacementRows/BillboardRow.h: CommonLib's NiBillboardNode face-mode member is at the wrong offset here. Read +0x128.
- Gpu/BuildExecutor.cpp, ShaderPrograms.cpp, DrawPipelinesImpl.h: the pump, lane, completion queues and Completions are never destroyed, because work completes during teardown.
- Gpu/ImportRelocations.h: poll on every Present, not just DCLF frames.
- Gpu/TextureFeedback.cpp: an UPLOAD buffer cannot have UAV. Writes tolerate races.
- Gpu/Pipelines/ShaderPrograms.cpp: no -Od builds (they read per-frame cbuffers the epochs do not supply).
- Gpu/Pipelines/ShaderPrograms.cpp: on AMD D3D12, non-uniform ref indexing loops (17 vs 9 ms), so refs are forced uniform.
- Gpu/Pipelines/DrawPipelinesImpl.h: skinned eye meshes omit TEXCOORD2, so the pulled stage supplies zero.
- Gpu/Pipelines/DrawPipelinesImpl.h, MainPipelines.cpp: opaque write mode 1 vs alpha-tested 10. LOD techniques 9/13/15/18 use write mode 1.
- Gpu/Pipelines/DrawPipelines.h: CS's ShadowmapCascadeRasterizerFix per-cascade copies exist only while the view is drawn. CS swaps the deferred blend states between StartDeferred and ResetBlendStates.
- Gpu/Pipelines/DrawPipelines.h: address-sourced cbuffers cost about 40 registers on NVIDIA. Pushed addresses need 8-byte alignment. PhysicalStorageBuffer loads assume 16-byte alignment.
- Gpu/Pipelines/DrawPipelines.h, DrawPipelines.cpp: D3D12's SV_InstanceID excludes the first instance, hence the instance push word.
- Gpu/Pipelines/DrawPipelines.cpp: on descriptor buffers BasicRHI pushes only 128-256 B. The draw's 96 B must come first.
- Gpu/Pipelines/ShadowPipelines.cpp: the D16_UNORM depth bias constant matches what D3D11 means.
- Gpu/Pipelines/EngineStates: decal bias modes (FUN_1414b3bb0, FUN_1414b3d50) and the ToggleDepthBias byte.
- Gpu/Pipelines/SpirvReflection.cpp: GLSL.std.450 literal operands can look like ids.
- Gpu/Pipelines/FamilyPipelines.cpp: the depth vertex stage is left out of the recipe key so old cache keys stay valid.
- Handoff/PrimaryCull: the NiSwitchNode index is read at AE's offset (CommonLib is wrong). Process1's vtable is shared by every list process. Stood-in roots' kAccumulated must be written or FUN_1402cff60 stops animating them.
- Handoff/PassCapture: the hooks do not chain, so the sites must be the engine's own. Group renderers can be created mid-registration.
- Handoff/SceneLists.cpp: the ClearList, BuildSceneLists and CullList detours must install together. Never reference a root the snapshot did not hold. The TES worldspace offset differs on SE and AE.
- Handoff/ShadowViews.cpp: RenderShadowmap binds a target only for descriptors that clear.
- Engine/EngineAccess.h: NiSwitchNode fields must be read at AE offsets (CommonLib's sizes are VR's). Node flags are read atomically.
- Engine/Walkers.h: walk children only to _freeIdx, never with a range-for over GetChildren().
- Engine/EngineReadWindow.h, WindowWriters.h: the cell animation and animated-object jobs write inside the window. The window is suspended for them after sampling.
- Engine/Capture/SceneEvents.h: switch events are taken only on the render thread (a QueuedTree crash on a freed child).
- Engine/Capture/SceneEvents.h: leaf updates only in the world. The projectile pool's master clone sits at refcount 0.
- Engine/Capture/SceneCapture.cpp: RaceMenu-morphed partitions sit on DYNAMIC buffers DXVK cannot import, so DCLF copies them. Overlay relinks bring a new skin instance's shown flags.
- Engine/Capture/ConstantMirror.h: the engine writes some constants after Unmap. DXVK keeps the mapping valid until the next Map.
- Engine/Family/EngineDrawFamily.cpp: an odd index range reads 2 bytes past the end, which is safe because DXVK pads buffers to 4 bytes.
- Engine/Family/EngineDrawFamily.h: header registers (t16 and up) are bound on DXVK's own context, so the reads hold under the flush latch.
- Engine/HookSites.h: half installs once stalled the retirement chain, so a conflict turns DCLF off.

## 3. Possible bugs or dead code noticed

- Gpu/FrameValues.cpp, the parity check of moved-only sampling (about line 2080 at HEAD): `(void)a_job;` means the parameter is unused.
- Handoff/ShadowViews.cpp ShadowGlobal: a doxygen line and a `//` line said the same thing with different addresses (0x142033498 vs 0x2033498). I merged them into one comment with the full address. Worth confirming that the code reads the right one.
- Handoff/PrimaryCull.h / PrimaryCull.cpp: the class holds the cut, scene lists and ownership state together. Not a bug, but it is the main reason the comments were so long.
- Engine/EngineDrawFamily.h AddDrawTaker: a comment said ImageSpace keeps its own DrawTriShape detour "for now". That is a duplicate detour candidate. The comment was cut.

## 4. TODO/FIXME lines found

None in the slice.

## 5. Coverage

- Done: every `.cpp`/`.h`/`.inl` under Core/Gpu (Pipelines and PlacementRows included), Core/Engine (Capture and Family included) and Core/Handoff. That is 84 files changed, none left untouched or half done.
- Comment lines before and after, counted as whole-line plus trailing comments over the whole slice: about 5,177 before, 2,948 after. The diff is 2,556 insertions and 4,504 deletions.
- Each file passes the check that code with comments and whitespace removed matches HEAD (scratchpad `verify.py`). New trailing comments use two spaces before `//`. Doxygen continuation lines keep the block's indentation.
- Tools and specs: `C:\Users\matth\AppData\Local\Temp\claude\f5be53fdb6b18c23\c05c210a-3101-4ded-aeb9-5047fe31b158\scratchpad\comments\gpu-engine-handoff\`.

## Quote: original FrameValues class doc (FrameValues.h)

```
@brief FrameValues(N) (dclf-architecture.md, "FrameValues"): the values a frame's draws need to be its own - the
claimed members' placements (BindlessPlacement), the skins' palettes and the objects' shading (BindlessShading) - made on
DCLF's pool while the frame runs, and waited for by the GPU, never by the render thread. The scene's tables hold none of
them: these are the only ones drawn with.

Each frame the render thread kicks one producer (Kick, at the scene's start) with the placement plans the walks made since the last
(PlacementPlan: a delta of the placement set, made only when it changes), which the producer applies in order onto the set it keeps
(its movers and roots at stable indices). The producers run one after another on the preparation pool:
- they sample the kept movers and roots that moved, and the slots the walks wrote in full (once), across the pool, under
  engine-read leases: a skin's palette after the engine's palette update (which the native draw's bone setter would have run),
  into the rows kept by object slot and the palette rows kept by block (journalled KeptArrays); and the shading of the slots the
  walk and the accumulate phase named (ShadingItem), with the shading fields' values the frame's start captured (the wetness);
- what moved is what the move writers pushed (FrameValueMoves, the producer the queues' one consumer), looked up in an index from
  move key to the kept movers and roots, patched as plans add, change and remove them: a mover or root is sampled when one of its
  keys moved this frame or the last, when a plan added or changed it, and on the frame after a sample of it changed a row or palette
  (so a skin's previous palette and a placement's previous world settle once its motion stops); a palette rows class's items every
  frame, their blocks the class's registration's (Api::PaletteRowsContribution: particle quads face the camera);
- send the frame's buffers the runs they lack on the dedicated upload queue, then signal the frame-wait timeline to the frame's
  sequence number (always, also after a failure), which every batch of the frame waits for (GraphHost's frame wait).

The features' registered pools (Api::FrameValuePool, Extensions::RegisteredPools) run beside the built-in row sets, after them: their
frame-start reads after the built-in ones, their kicks after the built-in inputs, their samples after the built-in sampling, their
sends after the built-in sends.

Each of the row sets (the three built-in ones: placements, palettes, shading; then the registered pools', Features/Fades' seeds the
last) is a pooled version (D6, org::services::BackingPool on DCLF's retirement clock, the render thread its one acquirer): Kick leases
a backing holding every row, preferring the one that holds the most (its contentEpoch), and the producer sends it the journal's runs
since then. The frame's draws read those backings (the frame record's t123, t122 and t121, and the culling's latches); EndFrame binds
them to the frame's last submission, so no producer writes a backing while a frame reads it. No producer waits for the GPU.
```
