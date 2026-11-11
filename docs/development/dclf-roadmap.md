# DCLF roadmap

The work on Drawcall Limit Fix (DCLF) that is not built yet: plans, designs and follow-ups, each with its current state.
What is built is in [dclf-architecture.md](./dclf-architecture.md); how to check a change is in
[dclf-validation.md](./dclf-validation.md); defects and accepted differences are in
[dclf-open-defects.md](./dclf-open-defects.md). An item moves out of this file when it is built.

Each item gives:
-   **Goal**: what it achieves.
-   **Why**: what it costs today, with a measurement where there is one.
-   **State**: not started, designed, or partly built (what exists, by file and symbol). Paths are under
    `src/Features/DrawcallLimitFix/` unless they say otherwise.
-   **Design**: a sketch or the constraints, enough to start from.
-   **Needs**: what must exist first.

## Priority

The standing directions decide the order:
1.  **Remove the engine's remaining work first.** DCLF is heading to a fully GPU-driven scene. The engine's walks, culls and
    waits that are left on the render thread come before everything else (the first section).
2.  **No reliance on engine culling.** Nothing on this list may read the engine's cull or registration results; where an
    item replaces one, the replacement is DCLF's own GPU cull.
3.  **Per-object view bitmasks.** A new view or class is a view bit tested at the GPU cull, never a per-view list or cut.
4.  **Lock-free.** Work that moves off the render thread moves into the async graph and lock-free publication (the scene
    lane, the snapshot builder, pooled versions), not behind a faster mutex.

After the engine's work: the scene lane and the builder (second section), then GPU cost, then coverage of the classes and
views still drawn by the engine, then tooling. Outside DCLF comes last.

---

## 1. Engine work left on the render thread

The census after the last round: about 110 engine-kept passes a frame, none a DCLF member's. Main about 75 (water, sky,
grass, about 3 particle effects), reflection faces only the sky (about 21), sun cascades about 5. Whiterun's census of
2026-11-11 (`menus-org1.env`, before the menus open: about 69 a frame) had no shadow view's pass at all: main (water, sky,
master particles, Lighting under billboards, one skinned effect variant), the reflection faces' sky, and four hidden
first-person parts in main depth. The race menu adds the player's face shapes (`lighting: eligible, no record`, BSDynamicTriShape
under `BSFaceGenNiNodeSkinned`) in main, main depth and focus shadows, about 20 passes a frame. Not yet read why; the likely cause
is that the menu's edits reach the shapes by another writer than the head's morph job, so Features/Faces has no snapshot.

The ORG residue census of the p5-all tour (2026-11-11, `1111-072658-p35-p5-all-census-w3`; p5/flush.md "DCLF's share") agrees: in
a settled window DCLF leaves the engine about 3 particle effects a frame in Whiterun (master particle systems, 1.4) and 9 skinned
effects in Blackreach. The skinned effects, like the one skinned effect variant above (dog eyes), were the Effect port reading the
renderer data's vertex description instead of the shape's (fixed: `GeometryRecord::shapeVertexDesc`). Everything else DCLF leaves
is in the window after a load. Scenarios: `p5-all-census-dclf.env`, `-take.env`, `dclf-residue-census.env`.

### 1.1 The residue without traversal: stop queuing the list jobs

-   **Goal.** The engine's list jobs (`ListAccumulationJob`) are no longer queued for the main camera, and the render thread no
    longer waits at their `Finish`.
-   **Why.** `CS.DCLF.ListJobsWait` (`Core/Handoff/PrimaryCull.cpp`) is the engine's `JobList::Finish` over the main
    camera's scene-list cull jobs, waiting on the residue left in the lists: about 193 entries per build in motion, 0.17 ms
    median, 1.6 ms worst. With the engine's other job waits (`JobList::WaitForCompletion` under
    `CalculateAndDrawShadowCasterLights` and `Main::PreparePlayerView`) it is about 0.5 ms median, 2.6 ms worst.
-   **State.** Partly built. DCLF's entries leave the lists (`PrimaryCull`'s `Process1` stand-in and the `AppendVirtual`
    hook); decals are members. The per-reason counters of entries kept in the lists say what remains. Not built: the flat
    pass for what stays native, and dropping the jobs.
-   **Design.** What stays native (effects and blended variants not ported, billboards not ported) is found visible by a flat
    frustum pass on a worker, with no traversal, and handed to the engine's registration (`AppendVirtual`). Then the list
    jobs are not queued. The extra list, room and portal visibility, and the list processes' camera and planes stay. Each
    class DCLF takes over (sections 1.4, 1.5, 4) shrinks the flat pass's input.
-   **Needs.** Nothing for the flat pass; it gets cheaper with every class ported.

### 1.2 The water reflection: what the engine still culls in the cube faces

-   **Goal.** The engine's cull for the water reflection's two cube faces reaches nothing DCLF draws.
-   **Why.** `TESWaterReflections::Update` once cost 1.66 ms median, 5.3 ms worst (two `BSCubeMapCamera::RenderFace` a frame), with
    0.75/4.1 ms of face cull (about 1,100 `ProcessObject` a frame). That was measured before the LOD add-roots were skipped.
-   **State.** Built for every root DCLF draws. The cube camera has no cell roots: its roots array (`+0x188`) is filled only by
    `FUN_1405275e0`, called four times, all in the update (AE `0x14052080d` LOD land `0x14315b898`, `0x140520826` LOD objects
    `0x14315b8a0`, `0x14052083f` LOD trees `0x14315b880`, `0x140520854` the sky root `*(TES+0x100)+8`; checked in Ghidra
    2026-11-11: the three globals are written by the terrain manager and read by `BGSTerrainChunk`, `BGSDistantObjectBlock` and
    `BGSDistantTreeBlock`). `BSCubeMapCamera::RenderFaces` (`0x1414ed920`) culls only those roots into its accumulator. DCLF
    skips the three LOD add-roots while it draws them (`ReflectionFaces::AddLodRoot`), adding back only the engine-only subset's
    colour roots under them, so the faces' engine cull is the sky root alone, and the sky's draws are Features/Sky's on the ORG
    path (`CS_ORG_SKY`, the `SkyReflections` anchor).
-   **Measure.** One run with `CS_DCLF_CENSUS=1`: `[DCLF] census view reflection faces:` names only `Sky: BSTriShape`
    (about 19 passes a frame in Whiterun), and `[DCLF] reflection faces (T4 ...): N updates, N without the tree root, N without
    the LOD roots (0 engine-only roots added under them)`. Anything else in the census view is residue. A Tracy capture of
    `TESWaterReflections::Update` would say what the sky root's cull still costs.
-   **Left.** The sky root's cull. Removing it needs the sky's CPU work (its geometry updates and registration) off the engine,
    which Features/Sky keeps by design.

### 1.3 The LOD roots out of the main camera's cull

-   **Goal.** The main camera no longer culls `TES::lodLandRoot` and the tree LOD root, or runs the LOD segments' `OnVisible`.
-   **Why.** DCLF draws object, terrain and tree LOD and withholds the engine's passes, but the cull still runs (CPU cost
    only, not correctness).
-   **State.** Not started.
-   **Design.** Skip the roots in the main camera's cull as the reflection skips its LOD add-roots. Whatever state the cull
    maintained (segment visibility, tree LOD's hidden instances, LOD fades) must already come from events or the GPU: check
    every DCLF reader first (`Features/Lod/LodSegments.h`, `Features/TreeLod/TreeLodMirror.h`).
-   **Needs.** A check that no DCLF reader depends on the cull's side effects.

### 1.4 The master particle systems

-   **Goal.** Torches, candles and street mist (`MPSTorchFire01.nif`, `MPSCandleFlame01.nif`, `MPSMistStreet01.nif`, ...) are
    drawn by DCLF's particle path. Blood impact particles are master particle systems too (4.3).
-   **Why.** They are among the engine's remaining main passes, and DCLF's blended draws go before every remaining engine
    blended pass, so they are drawn in the wrong order where they lie behind a DCLF surface (dclf-open-defects.md).
-   **State.** Built behind `CS_DCLF_MASTER_PARTICLES` (default off) and run in game (scenarios
    `tools/cs-ab/scenarios/master-particles-0.env` and `-1.env`): the shots match the switch off, and the idle masters held with
    three references are the engine's (an emitter's value node names them: `<- MASTER REFS` counts them apart from DCLF's pins).
    What exists:
    -   `SceneTracker::PushAttached` and `PushDetached` take the node whose array holds the child, and `NoteEdit` takes
        `(child, parent, inWorld)`. SceneWriterFramework's child events pass their parent.
    -   `Api::UnheldRootsContribution` (`Api/ObjectContributors.h`, `InstallContext::RegisterUnheldRoots`): roots listed as
        category nodes filed under themselves, with no `held` entry and a null `CategoryCapture::Root::root` and
        `AlwaysRenderRoot::root` (the key is `object`). The capture pins each root only until its first application.
        `Coordinator::OwnRoot` refuses an unheld root. A gone unheld root is not walked; its exit was an event.
    -   The masters are not in the category signature. Their owner's child edits count `SceneTracker::UnheldEdits` (not
        `CategoryEdits`), and an unheld root's detach forces no full capture (`Event::unheldExit`). After such an edit
        `CaptureCategories` derives the capture from the last one with the unheld roots gathered again (`DeriveUnheldCapture`: no
        cell or portal graph walked), and the coordinator's diff sees only the masters that came and went. A new owner is a full
        capture. The contribution's enter and leave are taken only on a frame that gathers the roots, from the gather to
        `PushCategoryRoots`.
    -   `SceneCapture::SetDrawnRoots` retires a replaced set through `SceneTracker::RetireSet`; `InWorld` reads it inside a
        `SceneTracker::SetRead`, and `RetireCategoryNodes` frees it once no read is in.
    -   `Features/Particles/MasterParticles.cpp`: the gather (the manager's children under its `BSReadWriteLock`, the global
        + 8), the removal hook, the counters and, with `CS_DCLF_STATS` only, the observers `<- MASTER SET` (a master left with no
        event) and `<- MASTER REFS` (an idle master held past removal for 600 frames). `<- PARTICLES` is the particles' parity line.
    -   Lock order: the capture's gate, then the manager's lock. `FUN_140e2a520` (the removal) runs on a job thread with no lock
        held (its one caller `UpdateParticleSystemManagerJob`, queued per slot by `FUN_1402c75c0` / `FUN_1402c76c0` under the
        queue's spin lock only), so the removal hook never holds the manager's lock while it waits for the gate.
-   **What the engine does** (Ghidra, AE 1.6.1170 / SE 1.5.97, the same code at the same offsets):
    -   The manager (`gParticleSystemManager`, AE ID 411270 / SE 524630) is ShadowSceneNode child 5. Its `AttachChild`
        override (AE `0x140e29f90`) takes only a `BSMasterParticleSystem` whose slot (`+0x158`) is set, and stores it with
        the array's `SetAt` under the lock at the global + 8. It never writes the master's parent.
    -   `UpdateParticleSystemManagerJob` (AE ID 19206) runs `FUN_140e2a520` (AE ID 76601 / SE 74853) per slot on job threads:
        `if (master+0x166 && refCount < 3 && FUN_140e2b430(master) == 0) children.SetAt(index, null)`, else the master's
        `UpdateDownwardPass`. The `SetAt` call at +0x69 (`NiTObjectArray::SetAt`, AE ID 70322 / SE 68970) is hooked; it fires
        no child edit. `FUN_140e2a5c0` (AE ID 76602 / SE 74854), the same test in a loop, has no caller in either runtime.
    -   The world reset (`FUN_140648cd0` +0x584, AE ID 36601 / SE 35593 +0x475) calls the remove-all on the manager:
        SceneWriterFramework's remove-all detour fires a detach for every master, so no reset thunk was needed.
    -   A master's drawn content is its NiNode children (the MPS model's `NiParticleSystem`s). Emitters feed it through
        `BSParticleSystemManager::sub` (AE `0x140e2a2f0`): the source's addon `BSValueNode` is held in the master's `+0x128`
        array (count `+0x138`), found by `FUN_1402df8a0` when a reference's or a temporary effect's 3D attaches.
-   **Risks.** A lasting reference (guarded: no `held`, no root reference, no root owner; `<- MASTER REFS` watches). A removal
    through the manager's own `DetachChild` overrides does not wait for the capture's gate: none is known during play. The
    interior route is not checked: a master listed in a portal graph's `alwaysRenderChildren` stays unheld (the gather runs
    first). A master has no fade node. The removal hook is one more trampoline call (the plugin allocates 4 KB).
-   **Residue.** They are the steady Effect draws in the residue census (Whiterun, about 3 a frame: `DrawDynamicTriShape (DrawData
    copy)` from the dispatcher's +0x101, geometry type 1). `p5-all-census-dclf-take.env` runs the tour with the switch on.
-   **Needs.** Before it is on by default: a run of the unheld roots diff (its count, "unheld roots diffs", is in the C2 parity
    line with `CS_DCLF_PERSISTENT_PARITY`), and the test that no master stays listed after its torches are gone (`master
    particles ... listed`).

### 1.5 The remaining residue classes: grass, water, sky

-   **Goal.** DCLF draws grass, water and the sky, each as an object class with its own view bits.
-   **Why.** They are most of the engine's remaining main passes (about 75 a frame with the particle effects), and each
    keeps an engine walk.
-   **State.** Not started. Precipitation is in the same position: the `BSParticleShader` techniques (rain, snow, Blackreach's
    spores; residue census `Particle` vs `0x4`, about 10 a frame in Blackreach) are drawn by the engine through the grouped
    alphas (AE 107673); DCLF draws only the precipitation occlusion mask.
-   **Design.** Take them class by class in measured cost order. Grass interacts with Grass Collision and
    `GrassOptimizations`; water with the water reflection (1.2) and its shaders; the sky also fills the reflection faces.
-   **Needs.** A census of each class's cost before choosing the first.

### 1.6 Bone palettes on the GPU

-   **Goal.** The GPU computes the skins' palettes from bone world transforms against inverse binds uploaded once.
-   **Why.** FrameValues runs the engine's palette update (`FUN_140e4ff90`, `Core/Engine/SkinPalettes.h`,
    `Engine::Address::kUpdateSkinInstance`) per skin every frame, inside the engine-read window.
-   **State.** Not started.
-   **Design.** A worker gathers the bone world transforms of animated actors; a compute pass writes `DCLFPalettes` (three
    float4 rows a bone, previous palette beside it, packed eye-relative). Static skins then cost nothing per frame.
-   **Needs.** Dirty-transform events (1.7) for the gather to be O(changes).

### 1.7 Dirty-transform events

-   **Goal.** Movers are known from the engine's writes, not sampled every frame.
-   **Why.** FrameValues places every mover every frame. Detecting a change by comparing transforms costs as much as reading
    them, so the only cheaper source is being told.
-   **State.** Not started.
-   **Design.** Mark an object dirty where the engine writes its world transform (`NiAVObject::UpdateWorldData` and the
    downward-pass paths; the full set, including Havok and animation, needs reverse engineering). That is a store per updated
    node on the update thread. FrameValues then samples only the dirty objects; a static scene costs nothing per frame.
-   **Needs.** The reverse engineering of every writer. 1.6 builds on it.

### 1.8 A finer actor move event

-   **Goal.** An actor's move event fires only when it moved.
-   **Why.** Every loaded actor has a move event every frame.
-   **State.** Not started.
-   **Design.** Raise the event below `UpdateAnimationGraphManager`, where the actor's pose or position actually changes.
-   **Needs.** Part of 1.7's writer survey.

### 1.9 Event-carried hidden state

-   **Goal.** A node's hidden bit reaches the tables with its event, closing the flip race after the drain.
-   **Why.** The behaviour graph's visibility controller flips equipment (`Shield:0`, a broom) during the scene work, and DCLF
    follows a frame late (dclf-open-defects.md).
-   **State.** Not started. `Features/HiddenNodes` captures the patched stores; `CS_DCLF_HIDDEN_WATCH` names an uncovered
    writer.
-   **Design.** The store hooks carry the new value in the event; the tables apply it in event order rather than re-reading
    the node after the drain.
-   **Needs.** Every writer covered (the shield's equip-code store is not).

### 1.10 Full evaluations made incremental

-   **Goal.** A reset, a load, a rescan or a live toggle no longer runs a full evaluation.
-   **Why.** These run `Coordinator::BuildFullOrder` (`Core/Scene/Coordinator/Walk.cpp`; `Walk.h`'s `fullEvaluation`),
    which re-evaluates everything.
-   **State.** Not started; every other table change is already incremental (slots, delta walk, structural events).
-   **Design.** A load becomes the events of the cells it attaches; a toggle re-evaluates only the classes it enters (the
    classification caches already know their inputs, `Coordinator::InvalidateVerdicts`).
-   **Needs.** Nothing.

### 1.11 Render-thread leftovers

Small pieces still on the render thread, each with its route off it:
-   **The mirror's portal-root pushes and `CaptureMirrorRequests`** stay at the frame's start, because the capture races other
    threads' attaches inside the engine-read window (73-277 per 600 frames, `SceneTracker::TakeWindowEdits`). The race is the
    baseline's; closing it is what lets them move.
-   **`CellRoots`** (`Core/Render/Readback/Visibility.cpp`) walks the object root's children every frame (about 26 nodes). An
    object-root edit counter in `PrimaryCull` would let it walk only on an edit.
-   **`UpdateActorRate`** (`Features/Actors/ActorRate.cpp`): its bound tests need the frame's last shadow view and must finish
    before `EngineReadWindow::Close`. A kick after the shadow epoch's commit (where `ParabolicExclusionSlot::Publish` runs)
    would let a leased task do them.
-   **`TerrainShadows::UpdateShadow`**: 0.40 ms median, 0.46 ms worst, every frame. Off the render thread, or event-driven.

---

## 2. The scene lane, the snapshot builder and publication

### 2.1 The scene lane's cost

-   **Goal.** An average lane pass of 0.4 ms (was 559 us) and the worst 30 passes under 3 ms (was 6.1).
-   **Why.** Burst passes' lane cost (worst 30 passes): `Scene.Detaches` 1.02 ms, `Mirror.Apply` 0.62,
    `Capture.ShadowSets.Index` 0.46, `SunCandidates` 0.42, `EvaluateStream` 0.39, `PrefetchBuffers.Wait` 0.32. In an average
    pass `MaterialCaptures` is the largest step, about 60 us.
-   **State.** Partly built: most steps are split into a parallel prepare and a short apply. Not yet split:
    -   `ResolveMaterialBinding` (`Core/Scene/Coordinator/MaterialBindings.cpp`) on the lane;
    -   the walk merge's `geometryIndex` lookup, which needs an index frozen per round;
    -   the shadow build's pre-phase, which needs a two-stage `ShadowBuildStages` like `MainBuildStages`
        (`Core/Render/Payload/MainBuild.h`).
-   **Needs.** Nothing.

### 2.2 The snapshot builder's O(whole)

-   **Goal.** Every build step is O(changes).
-   **Why.** Two steps are still O(whole): the main build's last stage copies the primary segment's payload whole into the
    other segment (`other = out`, `Core/Render/Payload/MainBuild.cpp`), and `PackPipelines` compares every pipeline (O(P)).
    Replaced builds also cost builder CPU: the publication gate lets newer work replace a sealed, unpublished build, so more
    revisions are made and abandoned (sealed 486 to 544 in a comparable window), while GPU and upload costs fall.
-   **State.** Not started.
-   **Design.** The second segment shares the primary's chunks or catches up from the same journal; `PackPipelines` packs
    from a pipeline change log.
-   **Needs.** Nothing.

### 2.3 Staged uploads become publication content

-   **Goal.** `CommitUploads`' staged batches go; nothing writes into a version after it is named.
-   **Why.** The commits still stage writes on the render thread (a memcpy into a `StagedUploadBatch`, submitted at the
    commit's end) into the version the adopted set names: the face positions, the tree rows, the fade roots with their
    programs and animation stamps, the index pool's copy list, the readbacks' arm headers, and a few zeroings. This is safe
    (copies are ordered on the queue) but breaks the rule that a version is immutable once named.
-   **State.** Not started. `AcquireStagedBatch` (`Core/Render/Payload/Uploads.h`) is used by
    `Features/TreeWind/TreeWindScene.cpp`, `Features/Fades/FadeCommit.cpp`, `Core/Render/Shadow/ShadowIndexPool.cpp`,
    `Core/Render/Readback/Visibility.cpp`, `Core/Render/Resources/FrameRecords.cpp` and `Features/Portals/PortalPrograms.cpp`.
-   **Design.**
    -   The face positions, tree rows and fade root rows become payload parts (more versioned parts built from their
        predecessors on the state graph, like the scene payload's seven), their deltas from their tables' journals, read by value. The tree rows get a journal by tree slot
        (`KeptArray`); today `UploadTrees` uploads them whole whenever the tree content epoch moves.
    -   The index pool's copy list becomes the publication's: the pool's update is a pure function of the geometry log.
    -   The per-frame rows (`TreeWindFrameRow` into `treeFrameBuffer`, and `fadeFrameBuffer`) move into the latch through a
        latched copy: the depth shape's `MainLatchedLayout` lists a `LatchedCopy` for each, `MainLatchedTargets` declares
        both, and the commit's staged writes become `latched(...)` writes. The plain buffers then go.
    -   Zeroings stay latched. The GPU-only state (`treeClocks`, `treeWindRows`, `fadeStates`) stays in the set. Once no
        write is left, `CommitUploads` and its batch pool go.
-   **Needs.** The readers first: the recordings bind these versioned buffers directly (`ShaderResource(*scene->fadeRoots)`,
    `ShaderResource(*scene.vertexStream)`, ...). A part needs its passes to read it by value (an SRV index or address through
    the latch), and the draws' face stream addresses pushed by BuildDraws (as the rows' block addresses are), not stored in a part.

### 2.4 Animated material values

-   **Goal.** An animated material value never makes a new material record or a publication.
-   **Why.** Each controller write is a new material record (`Tables::materialFrameVersion`), a rewritten row and a
    publication, and reaches the GPU a publication late. Material catch-up was about 300 KB a publication while such
    materials were in view, and these writes are why publications stay at about 1.7 a frame.
-   **State.** Partly built. `TexcoordOffset` is done another way: the main build lists every row carrying one
    (`TextureTransformList`, `Features/TextureTransforms/TextureTransforms.h`), FrameValues' `texture-transforms` pool samples
    them every frame, and `cs.dclf.z.texture-transforms` (`TextureTransformsCS.hlsl`) patches the installed rows first in the
    Z-prepass epoch (0.30 ms a frame on the pool in motion). Not built:
    -   the effect materials' `FalloffData` (0x38) and `BaseColor` (0x48) still go through records
        (`Features/Effects/EffectMaterial.cpp`, `kFalloff`, `kBaseColor`);
    -   the frame-sourced components: `RefreshMaterialSignatures` rewrites the frame-sourced floats into every slot of a
        signature whenever the frame's sources move (most frames), so `PublishTables` replays hundreds of 2.4 KB records a
        publication.
-   **Design.**
    -   **Effect values.** A FrameValues row set by material slot holding only the animated floats, sampled at the frame's
        start and caught up from a journal by slot. The draws read it through a frame record register (as t121-t123); the slot
        is the input's `y` (low 20 bits). The record keeps its evaluated values and stops moving. This changes
        `package/Shaders` (the Lighting and Effect stages under `DCLF_BINDLESS`) and recompiles the shader cache.
    -   **Scroll on the GPU.** Evaluate the controllers' closed form of time on the GPU from per-material constants captured
        once, so the `texture-transforms` pool and its reads go. First find what `BSLightingShaderPropertyFloatController::Update`
        (0x14150DDE0) and the effect controllers read (keyframe interpolator, linear rate, or both), what stops or restarts a
        controller (activation, `kActive`, cell unload) so the constants change only on events, and the controller clock
        against the frame's.
    -   **Epoch order (check first).** Epochs recorded before the Z-prepass epoch that read the material rows see last frame's
        offset. Confirm which do, then patch in the earliest (the latch carries the stage word,
        `BuildDrawsLatch::stageWord`), or let each pass compute its own value with the GPU scroll.
    -   **Frame-sourced components.** A per-signature row packed with the record (a frame's move writes one row per
        signature), or copy-on-write chunks for `SceneTables::materials`.
-   **Needs.** Measure first: the payload parts' versions (the `payload <part> versions` lines: KB uploaded and copied) and
    `frame data through the render thread` (`Diagnostics/Reports/DrawsFrameValues.cpp`).

### 2.5 The latch carries only per-frame values

-   **Goal.** Values that change per publication or rarely leave the per-frame latch.
-   **Why.** A latch block has a region per frame slot, and the epoch's latched-copies pass copies every value every frame,
    changed or not.
-   **State.** Partly built. Per-publication bytes are written once per frame slot and again only when their key moves
    (`LatchRegionsHeld`, `Core/Render/Payload/Latched.h`); the shadow arena stages only the 256-byte chunks that differ
    (`kArenaChunk`, `Core/Render/Shadow/ShadowEpochs.cpp`). Staged shadow constants fell from 4.0 to 1.5 KB a frame. Not built:
    -   **per-publication values** (the BuildDrawsLatch's payload block, the frame record's object and extras registers) as a
        small immutable block per payload source, pooled like a part and named once by value;
    -   **rare values** (the frame record's texture descriptors, the captured D3D11 constant blocks, the engine's structured
        frame buffers copied whole every colour commit) as pooled versions published only when their bytes change (the
        `ConstantMirror` knows), the latch carrying only an index;
    -   `RefreshFrameConstants` split: the lighting merge and technique fog read the same Prepass engine state as the
        camera-dependent part;
    -   `FrameData::Note(std::string_view, size_t)` (`Core/Gpu/FrameData.h`) builds a string key per write (about 150-170 a
        frame); an interned key per target removes the allocations.
-   **Design.** The recordings are fixed per revision, so every pooled value still reaches the GPU by value through the latch;
    nothing can be bound in a recording.
-   **Needs.** Nothing.

### 2.6 Smaller pool gaps

-   **Readback staging sets.** `HostEventRing::StagingSet` (`Core/Render/Readback/Readbacks.h`) is append-only: a set for a
    capacity no sealed set names is kept until the ring goes. Release it on the retirement clock once no sealed set names its
    capacity and no frame bound one that did. Low priority: it grows only with the event lists. The frame-slot ring itself
    stays (GPU-to-CPU, rewritten whole every frame, retired by reuse).
-   **First versions outside the pools.** A `GrowableBufferSet` Zero registration's first version comes from the pool, but
    Fresh registrations' first versions are made directly (`MakeVersioned(CreateWords(...))` in
    `Core/Render/Resources/Resources.cpp`, `Core/Render/Shadow/ShadowIndexPool.cpp`, `Core/Render/Main/FrameLayout.h`), so
    the pools' census and leak watch miss them. `GrowableBufferSet::Register` should make the first version through the
    registration's pool. Fixed setup-time buffers stay.

### 2.7 The CPU pipeline's sun cull: a sun entry sphere column

-   **Goal.** The sun's lane loop in the CPU pipeline's cull runs as a wide pass, like the frustum and portal passes.
-   **Why.** In the heavy exterior on a Zen 5 CPU, the sun costs about 125 cycles a lane and view, and it is the largest view
    left after the main camera (about 0.35 ms of pool time a frame). Most of that is scalar work on scattered memory: each
    passed lane reads its object's sun entry spheres (`CullFrame::sunEntry[lane.object]`, `Draws::OutsideSunEntryProcesses`)
    and its cascade inputs through the object index.
-   **State.** Not started. Tried and dropped: the sun entry drop as a gathered wide pass over the existing layout
    (`Core/Render/Direct/Cull.cpp`, `ShadowDropped`); the gathers cost more than the loop saved.
-   **Design.** The view batch builder (`Core/Render/Snapshot/ViewBatches.cpp`, `SceneLists.cpp`) writes a column per batch
    with each lane's sun entry sphere, contiguous by lane like the frustum's sphere columns, so the sun entry and cascade tests
    load it directly in `CullSimd.h` kernels. The column follows the lanes through the scene lists' deltas and compaction. The
    verdicts must match the current loop bit for bit (a `dclf:cpu-exp` check bit, as the frustum and portal kernels have).
-   **Needs.** Nothing.

---

## 3. GPU cost

DCLF on was about 0.5 ms slower than native when GPU-bound (depth segment +0.30 ms, world segment +0.14 ms; see
dclf-open-defects.md). These items cut DCLF's fixed GPU cost.

### 3.1 Plain indirect draws for the colour pass

-   **Goal.** The colour segment draws with plain indirect draws instead of device-generated commands.
-   **Why.** About 74 ns a sequence: 318 us for 4,291 sequences with nothing rasterized.
-   **State.** Done: the main, terrain, decal and forward buckets (`CS_DCLF_*_PLAIN`, on by default) draw each pipeline's range with
    one counted indirect draw and the pulled colour pipelines, which find their sequence by instance. Without device-generated
    commands with pipeline sets (AMD, Intel) they are the only colour draws (dclf-architecture.md, "Explicit DGC preprocessing
    and pipeline set versions"). Open: the first-person forward part has no buckets, so it needs the direct forward family there;
    without it the first-person view is not covered and the engine draws it.

### 3.2 Early builds on the compute queue

-   **Goal.** The culling and build dispatches run on the compute queue ahead of the raster work that consumes them.
-   **Why.** The Z-prepass's culling (about 45 us), the colour build and sort (about 50 us) and the shadow build sit on the
    graphics queue in front of their draws.
-   **State.** Not started. Only fade state (`Features/Fades/FadeStage.cpp`) and tree wind (`Features/TreeWind/TreeWindPass.cpp`)
    use `PreferQueue(org::QueueKind::Compute)`.
-   **Design.** Issue the Z-prepass's cull at the shadow epoch, the colour build and sort at Prepass, and the shadow build at
    `BeforeShadowMaps`.
-   **Needs.** Nothing beyond the graph's queue support.

### 3.3 Two-phase culling's fixed cost

-   **Goal.** The second culling pass (the HZB rescue) costs nothing when it rescues nothing.
-   **Why.** About 23-24 us a frame after the depth-pipeline buckets.
-   **State.** Not started (`IndirectDrawStats::cullRescuedByPhaseTwo` counts the rescues).
-   **Design.** Load and store less, or skip the pass when the build counts no rescue candidates (an indirect dispatch with a
    zero count).
-   **Needs.** Nothing.

### 3.4 Depth pipeline groups converging

-   **Goal.** Identical depth code yields one module and one Z bucket.
-   **Why.** Shader debug info embeds each stage's defines and source, so identical code yields distinct modules.
    `CS_DCLF_SHADER_DEBUG` is on by default: 58 depth pipelines of 95 main pipelines, against 35 with debug info off. The rest
    of the divergence is real (9 vertex modules, and cull mode).
-   **State.** Not started.
-   **Design.** Key the depth buckets on the code without its debug info (or compile the depth variants from one shared
    define set), keeping debug info for crash dumps.
-   **Needs.** Nothing.

### 3.5 Shadow raster state per draw

-   **Goal.** A shadow view's raster state is a value per draw, chosen with an indirect execution set.
-   **Why.** Views take one state from the table entry (`Core/Render/Shadow/ShadowBuild.cpp`, `modeRasterStates`). A view whose
    entry turns depth clipping off draws with depth clamped (`ShadowRasterState::depthClip`, BasicRHI's
    `RasterState::depthClip`, `CS_DCLF_SHADOW_DEPTH_CLAMP`); only a wireframe entry, or depth clip off on a device without
    `depthClamp`, stays native (`not ready (raster-state=N)` in the `[DCLF] shadow views:` line).
-   **State.** Not started.
-   **Needs.** The indirect execution sets' validation errors closed first (dclf-open-defects.md).

### 3.6 The forward sort at scale

-   **Goal.** The forward class sorts in bounded time however large it grows.
-   **Why.** `ForwardSortCS.hlsl` sorts per tile; it suits today's class sizes.
-   **State.** Not needed yet.
-   **Design.** A radix or bitonic sort with the same contract. The engine key's `+0x51` radius term and `qsort`'s integer
    comparison (groups within one unit unordered) are not modelled either way.

---

### 3.7 Point light selection once per object

-   **Goal.** Without Light Limit Fix, a draw's point lights are selected once per object, not by every wave of its pixels.
-   **Why.** `DCLFSelectPointLights` (`Common/DCLFPointLights.hlsli`) loops over up to 128 table rows in the pixel stage, though
    its inputs (the object's bound, the draw's shadow mask) are the draw's (dclf-architecture.md, "Point lights without Light
    Limit Fix").
-   **Design.** BuildDrawsCS, which already selects the object's shadow lights from the same bound (`LocalShadowMask`), writes
    the plain lights' table indices (seven bytes and a count: two words) into a per-object buffer of the frame, like the
    visibility words; the pixel stage takes the shadow rows from its mask (four rows at most, first in the table) and the plain
    ones from those words. The CPU pipeline's cull (`Direct/Cull.cpp`) writes the same words for its draws. The table goes into
    the build's latch. Not the vertex stage: it would run the loop for every vertex and add two flat interpolants.
-   **Needs.** The buffer's graph resource and its read by the colour draws (a pixel-stage register), and both culls.

## 4. Coverage: classes and views still the engine's

### 4.1 Effect variants not ported

-   **Goal.** Every effect variant the engine draws in the main view is DCLF's.
-   **Why.** Each unported variant stays native, keeps the list jobs (1.1) and draws after DCLF's forward epoch, out of order.
-   **State.** Partly built. Cloud lighting (`CS_DCLF_EFFECT_CLOUD_LIGHT`) and Falloff without blending (`CS_DCLF_EFFECT_FALLOFF`)
    are DCLF's (dclf-architecture.md, "The Effect shader in the forward epoch"). Not started, each with what it needs:
    -   **Sky objects** (bit 45, hint 0x12, geometry group 13). The sky family (`Features/Sky`, `CS_ORG_SKY`) latches only the
        `BSSkyShader` techniques' batch (`0x5C00005D`-`0x5C000064`), so an effect sky object is still drawn by the engine's
        group 13 effect batch. Needs SetupGeometry's own cull and alpha setup (`FUN_14150bc80` with the alpha test) and a draw
        in the sky's place (before the opaque world), so the sky family is the natural owner, not the forward epoch.
    -   **Wireframe** (bit 48): SetupGeometry sets depth mode 3 and the rasterizer's fill mode 1. BasicRHI has
        `rhi::FillMode::Wireframe`, but DCLF's `PipelineKey` has no fill-mode bit. No vanilla content is known to set it.
    -   **Projected UV** (pass bit 17): SetupGeometry writes VS `MatProj` from the transpose of the pass geometry's `+0x16c`
        matrix and binds a texture from `BSGraphics::State +0x48` (`+0x10`) with filter and address 3. Needs the writer of the
        geometry's `+0x16c` and that texture as a frame texture.
    -   **Unrecognised blend pairs**: `FUN_14150bc80` leaves the blend state as the previous draw left it, so the result
        depends on the engine's draw order. Not portable as a fixed key.
    -   **Membrane** (pass bit 15): SetupGeometry takes `BSEffectShader__sub` instead of its usual body (the TESEffectShader
        membrane: normal and noise textures, `MembraneRimColor`, `MembraneVars`, `TexcoordOffsetMembrane`). The pass is an
        effect pass on an actor's geometry. Needs that function's port and the actor pass.
    -   **Rigid skins** (a skin instance whose pass has no Skinned bit): built behind `CS_DCLF_EFFECT_RIGID_SKINS` (default off),
        not yet run in game.
    -   **Strip and mesh particle systems**: strips are built on the CPU each frame (`FUN_140d76d80`, a triangle strip into
        the dynamic ring); mesh particles draw an instanced mesh. Both need their own particle rows (`Features/Particles`).
    -   **Skins under billboards, nested billboards, Lighting leaves under billboards**: the billboard row turns one node
        from one base transform (`BillboardRow.h`); a nested chain needs a row per node, a skin needs the turn applied to
        its palette, and a Lighting leaf needs the turn in Lighting.hlsl's vertex stages (depth, colour, shadow) and a place
        for the mode, since a Lighting record's bits 4-7 are refraction's.
-   **Order.** By the residue census (below), most passes first.
-   **Census.** `CS_DCLF_CENSUS=1` (dclf-validation.md, "The registration census"). The lines to read are
    `[DCLF] census class effect: effect-variant (<variants>)` and `[DCLF] census class effect: effect-opaque`, with their
    passes a frame and top geometries. Run it before and after a change in the same cells, with the variant's switch on and
    at 0 (`tools/cs-ab/scenarios/effect-variants-census.env`, `effect-variants-census-off.env`).
-   **Design.** Each is an `Api::ObjectClass` contribution in `Features/Effects` or `Features/Billboards`, or a variant bit
    dropped from `EffectVariantsOf` behind its own switch.
-   **Without Light Limit Fix**, the effect Lighting variant can read the frame's point light table the Lighting draws already
    read (dclf-architecture.md, "Point lights without Light Limit Fix"): `Effect.hlsl`'s PerGeometry lights from the table,
    picked by the object's bound.

### 4.2 First-person refraction

-   **Goal.** `HeatRefraction` in first person (Flames) is drawn by DCLF.
-   **Why.** One engine pass a frame while Flames is drawn.
-   **State.** Not started. What the engine does: `Main::RenderFirstPersonView` (AE `0x1414cb6d0`) submits the first-person
    accumulator, then calls `FUN_1414b4860` on it, which draws its group 5 (the refraction normals) as the main view's does.
    The leaf is a Lighting `kRefraction` shape under an `NiBillboardNode` in the hand's NIF.
-   **Design.** A billboard turn in the Utility refraction stage, and a refraction pass in the first-person colour epoch (the
    first-person camera and depth range, `kPOST_ZPREPASS_COPY`, the begun/drawn flags so the main view's normals are not
    cleared again). In the CPU forward family (`CS_DCLF_FORWARD_DIRECT`) the first-person site needs a refraction view as the
    main one has (`Core/Render/Direct/Sites.cpp`).
-   **Needs.** Lighting leaves under billboards (4.1): the classification admits only effect shapes
    (`BillboardTurnedLeaf`), and the refraction record uses bits 2-7, where the Effect family keeps the billboard mode, so the
    mode needs another place (a shading-row component refraction does not read). The first-person view's slots also exclude
    refraction (`ViewEpoch.cpp`). First person itself is on by default (`CS_DCLF_FIRST_PERSON`), except on a GPU-driven graph
    without device-generated commands and without the direct forward family (`GraphConditions::firstPerson`: its epochs and slots
    are left out there).

### 4.3 Blood impact particles

-   **Goal.** `pBloodSplat02`, `pBloodDrops06` and `pBloodLines` are drawn by DCLF.
-   **State.** Covered by 1.4: no separate switch. These are the particle systems of master particle systems, so
    `CS_DCLF_MASTER_PARTICLES=1` takes them (scenarios `tools/cs-ab/scenarios/blood-particles-0.env` and `-1.env`, not run
    yet; 1.4's scenarios ran). `CS_DCLF_BLOOD_PARTICLES` was not added: a blood-only filter over the masters would be a second
    listing rule for the same roots.
-   **What the engine does** (Ghidra AE 1.6.1170, and the game's meshes):
    -   A melee hit (`Actor::CalculateCurrentHitTargetForWeaponSwing`) and `BGSImpactManager::PlayImpactEffect` spawn a
        `BSTempEffectParticle` (`BSTempEffectParticle::Spawn`, AE `0x1404a7320`) with the impact data's model, for the actor's
        cell. The effect keeps the cell 3D's child 4 (the Dynamic category node, `+0x48`). Once its clone task is done, its
        update (`FUN_1404a8150`) attaches the 3D (`FUN_1404a78f0`) under that node, through `AttachChild` or the task queue's
        node attach. Both reach SceneWriterFramework, so DCLF tracks the impact's own 3D like any Dynamic member. TES pools up
        to three clones per model (`TES+0x290`, `FUN_1401a3c90`); a reused clone is attached again. The effect's
        `Detach` calls the parent's `DetachChild`.
    -   Before the attach, `FUN_1402df8a0` walks the 3D. Each addon `BSValueNode` whose addon form has the master flag
        (`+0x7a & 1`) gets its master (`FUN_140256390` loads the MPS model and attaches it to the manager) and is held as one of
        the master's emitters (`BSParticleSystemManager::sub`).
    -   The impact models (`meshes\effects\impacteffects\bloodspray*.nif`) hold the emitters (`pBloodLines02`,
        `pBloodDrops08`, ...) and value nodes named after their masters (`MPSBloodSprayTiny05`, `MPSBloodSprayImpact03`,
        `MPSMeatChunk`). The three names in the census are only in the masters' models: `pBloodSplat02` in
        `meshes\mps\mpsbloodsplatimpact02.nif`, `pBloodDrops06` and `pBloodLines` in `meshes\mps\mpsbloodspraytiny03.nif`
        (`pBloodLines` also in `mpsbloodspraybigdrops.nif` and the DLC1 explosion). So they are under the particle manager,
        not "temporary effects outside DCLF's categories".
-   **Needs.** A run of `blood-particles-1.env`.

### 4.4 Object LOD's segments in occlusion

-   **Goal.** Object LOD is occlusion-tested per segment range, not as a whole shape.
-   **State.** Not started.
-   **Design.** A verdict per geometry slot, since the colour pass must draw exactly what the depth phases drew.

### 4.5 Tree wind details

-   **Loop 2's second `dt`.** Near trees with bones (the `FUN_140437e50` list) advance twice in the engine; `TreeWindCS.hlsl`
    advances one `DeltaTime` a frame. Never validated: no such tree was in the test scene.
-   **The near list's count cap.** Not replicated (`Features/TreeWind/TreeWindSeeds.cpp`). With more selectable trees in range
    than the cap, DCLF gusts them all where the engine gusts only the nearest.

### 4.7 Geometry the game can map (`UnstableBuffer`)

-   **Goal.** A shape whose vertex or index buffer the game can map (D3D11 `USAGE_DYNAMIC` or CPU access) is DCLF's.
-   **Why.** DXVK renames such a buffer on every discard map, so neither `dxvkGetInteropResourceInfo` nor
    `dxvkTrackInteropResources` imports it, and the object stays the engine's in every view (`Ineligible::UnstableBuffer`). The
    engine's own geometry creators make `DEFAULT` buffers (p4/p4.5-buffers.md), so these come from plugins: a RaceMenu body morph
    on test2. None appeared in the Whiterun census of this load order (2026-11-11, `menus-org1.env`: no
    `lighting: unstable-buffer` class).
-   **State.** Measured, not claimed. `Coordinator::NoteUnstable` counts each such object by cause (`Stats::unstableCauses`: a
    buffer the game can map, one DXVK refused otherwise, no buffers to resolve) and logs the first 16 with their buffers' usage
    (`[DCLF] unstable buffer: '<name>' stays the engine's: <cause> (<bytes> B usage <u> CPU access <flags>, ...)`). With
    `CS_DCLF_STATS=1` the coordinator's line ends `(unstable: M mappable, R refused, S no source)`; the census names their passes
    `lighting: unstable-buffer`.
-   **Design.** Re-import through the path pinless imports already use, with no graph rebuild (geometry is drawn by device
    address, not as a graph resource). DXVK: a tracked import of a mappable buffer (a new `DxvkOrgInteropTrackedFlagBits` bit)
    keeps its discard renames and reports each as a relocation holding the old slice, alive until released. DCLF: the
    relocation reaches `GpuResources`' `ImportRelocations` at the next Present (`ImportRelocations::Poll` runs at every Present on
    the render owner since 782c888bd, loads included), its geometry slots are marked
    (`relocatedGeometrySlots`), and `ResolveGeometrySource` acquires the new slice into the same slot (`staleGeometry`), as a
    defragmentation move does. The draws read the slice of the publication's frame, so an edit shows a frame or more late; a
    buffer discarded every frame refreshes its slot every frame. Gate it on the measured count: worth it only where the census
    finds such shapes.
-   **Needs.** The DXVK interop change and a load order that makes such buffers (RaceMenu body morphs) to test it.

---

## 5. Integrations

### 5.1 Light Limit Fix's local shadow cache under DCLF

The cache is off while DCLF is loaded (`LightLimitFix::LocalShadowsBlockedByDCLF()`). The three ways to make them agree on
which lights to draw are in dclf-open-defects.md,
["Light Limit Fix's local shadow cache is off while DCLF is loaded"](./dclf-open-defects.md#light-limit-fixs-local-shadow-cache-is-off-while-dclf-is-loaded).
Of those, consulting the cache's filter from DCLF's port (`Features/LocalLights/LightSelection.cpp`) through an Integrations
extension fits the direction best: DCLF keeps its own selection, with no reliance on the engine's.

### 5.2 Light Limit Fix's light array in its latch

-   **Goal.** Light Limit Fix stops copying its whole light array into a `maxLights`-sized latch region every frame.
-   **Why.** `ORGLightCulling` (`src/Features/LightLimitFix/ORGLightCulling.cpp`) makes its latch at `kLatchLightsOffset +
    maxLights * lightStride` per frame slot and writes the array every frame.
-   **State.** Not started. Outside DCLF.
-   **Design.** With stable light indices: a pooled backing per frame caught up by light index, the latch carrying the count
    and the backing's index. Without them, the gain is only memory: a backing sized to the frame's count. Either needs a
    retirement clock outside DCLF's (`Core/Runtime/FrameRetirement.h` is DCLF's), for example one owned by
    `RenderGraphRuntime`.

---

## 6. Tooling

### 6.1 A renderer choice: D3D11, Vulkan, Vulkan with DCLF

-   **Goal.** One setting chooses native D3D11, Vulkan (DXVK) without DCLF, or Vulkan with DCLF.
-   **Why.** Comparing the three today needs environment variables, which users cannot set from the game.
-   **State.** Not started. The choices exist only as debug switches: `CS_NATIVE_D3D11=1`
    (`src/DxvkLoader.cpp`, `DxvkLoader::NativeModeRequested`, read once) and `CS_DCLF=0` (`Core/Settings/Switches.cpp`: DCLF
    does not install). DCLF's per-class live toggles (`Core/Settings/Toggles.h`, `ToggleSet`) exist, but no master one.
-   **Constraints.** The backend must be known before the game creates its device (`Hooks.cpp`, `InstallEarlyHooks` calls
    `DxvkLoader::Load`), so a backend change applies at the next launch. DCLF needs Vulkan. Switching DCLF off at runtime
    would have to withdraw every claim and hand every withheld pass back to the engine in one frame; a restart is the simple
    first version.

### 6.2 Nsight Perf: force a re-record when a capture arms

-   **Goal.** Nsight Perf measures DCLF's graph passes in a stationary or turning run.
-   **Why.** On Vulkan a graph pass's range is written into its command list when the pass is recorded. The persistent graph
    replays a revision's recordings while the revision stands, so a pass recorded before arming has no range; today
    DCLF's passes are measurable only while the scene changes (`CS_DCLF_TEST_MOVE`). See [nsight-perf.md](./nsight-perf.md).
-   **State.** Partly built: `src/Features/RemoteControl/NvPerfVulkan.cpp` pushes graph pass ranges while armed
    (`g_recording`). Arming does not invalidate the standing recordings.
-   **Design.** Arming (and disarming) bumps the graph's revisions so every epoch re-records once; the capture's first pass
    starts after every command buffer that can execute in it was recorded while armed, as it does now.

### 6.3 DXC compilation of Community Shaders' game shaders

-   **Goal.** CS's replacement game shaders compile with DXC and reach DXVK as SPIR-V, because the dxbc-spirv route costs GPU time (34% more
    instructions on grass).
-   **State.** Offline survey done, ingestion started. The plan, the measurements and the design are in [dxc-game-shaders.md](./dxc-game-shaders.md).

### 6.4 Checks not built

Diagnostics that would close gaps in what parity covers (the existing checks are in dclf-validation.md):
-   **A false-negative cull detector** (`CS_DCLF_CULL_VALIDATE`): draw every culled object against the final depth with
    LESS_EQUAL, no writes and early fragment tests, and flag any fragment. Today "no object wrongly culled" rests on
    `CS_DCLF_SET_PARITY` and image checks.
-   **BuildDraws parity for the depth segment.** `Diagnostics/Draws/BuildParity.cpp` skips depth-only epochs, because the
    Z-prepass's draws go to buckets; the depth segment has no template check.
-   **Tree LOD constants under capture parity.** Not compared with the engine's draws.
-   **Fade parity on one node** (the engine's state machine against DCLF's on the same node). The stand-in rules out a node
    both update; `FadeState::CheckPort` (on node copies) and the GPU-against-port log stand in for it.

---

## 7. Outside DCLF

-   **BasicRHI's present.** `sc_present` holds `timelinesMutex` across the present (`extern/BasicRHI/rhi_vulkan.cpp`; SARP and
    BasicRenderer only). CS's upload queue may not need DXVK's `LockSubmissionQueue`.
-   **Worst frames outside DCLF**, not investigated: single 17-57 ms stalls in `CS.ORG.FlushNativeProducer` (DXVK's
    `context->Flush()`), `RHI.Vulkan.QueueAccess`, Upscaling, and `GrassOptimizations::ApplyPending` spikes.
-   **Synchronous file reads in `Main::Update`.** The worst frames' unzoned main-thread time (about 6.7 ms) is reference
    promotion during cell attach (`BGSDynamicPersistenceManager` queue `15338` -> `QueuedPromoteReferencesTask::Func2`
    `15319` -> ... -> `TESWorldSpace` `20456` -> `NiFile` -> `ReadFile`). Next: confirm in a merged Tracy capture by zoning
    `15338`/`15319`, then decide whether the read can move to the loading thread or be read ahead. SkyrimEngineTelemetry
    listens on port 8087 beside CS's 8086; align captures on the paired `Main_RenderDepth` hooks (`mergert.py` paired them off
    by one when the captures started an anchor apart). The plugin does not create its report folder.
-   **BasicRenderer's pool telemetry**, lost when the pool moved to ORGModuleServices. BasicRenderer's adapter
    (`src/Runtime/Resources/VersionedBuffers/VersionedGpuBufferArtifacts.cpp`) keeps only `EmitPoolTelemetry`. Restore the
    process-wide gauges (`SARP.VersionedBuffer.PooledBackings`, `PooledBackingBytes`) by summing the per-pool gauges in the
    hook, and the exhaustion trace into `resource_lifetimes.csv` from the `Exhausted` event (rate limited to one a second).
    The per-build records (`BytesReplayed`, capacities, journal pages) and the `SARP_GPU_LIFETIME_TRACE` `version_release`
    line need new ORGModuleServices hooks: a build-stats callback from `BuildVersionedBuffer`, and a lease release event.
