# Comment cleanup: slice `scene`

Slice: `src/Features/DrawcallLimitFix/Core/Scene/` (all `.cpp`, `.h`, `.inl`), worktree `test-runner`.

## 1. Simplification or documentation targets

The biggest finding is structural. The same "pooled immutable snapshot, change log, replay onto the nearest free snapshot, spare caught up on the pool" pattern is built three times, and each copy is explained only by long comments.

- **SceneSet.cpp, PublishTables / TablesReplayPlan / CollectReplaySlots / ForLoggedColumns / SwapHeldOut / TablesSpare / TrimTablesPool.** This publishes the scene tables. A pool of snapshots is brought up to date by replaying the change, publication, material, extras and geometry logs, with a per-column "cause covers it" filter. A spare snapshot is caught up on the preparation pool, and some members are held out and swapped. Correctness depends on every writer calling the right `Note*` log for every column, and nothing in the code shows that rule. Suggestion: store the published columns in copy-on-write chunks, as `SharedChunks` already does for Lookups. Publishing then becomes a freeze plus a pointer copy, and the replay, spare and held-out swap code can go. Failing that, give the replay contract its own section in dclf-architecture.md.
  Original (TablesSpare): "PublishTables' spare (EntrySpaceTable's pattern): after each publication the free pooled snapshot nearest the logs is brought up to the published one on the preparation pool, from it alone (immutable: the spare holds it until the lane takes the spare), so the next publication replays one publication's changes on the lane, not every change since the snapshot's own publication. What it brings up is what the logs name: the logged and sided object columns (with the layer links' other ends), the material records, the extras rows, and the change, publication, material and extras logs (their chunks shared). The geometry columns stay the publication's (a lease a snapshot drops is let go on the lane), as does everything copied whole or kept by stamp."
- **EntrySpaceTable.h/.cpp** is the same pattern for the sun and light candidate sets: pooled snapshots, a log, runs, a spare on the pool, and a per-dispatch `State` so a stale job does nothing. **State/Set.h + SceneSet.cpp PublishClaims** is the same again for the claims pool (setPhaseLog replay). Suggestion: one generic "published versioned table" utility, or copy-on-write chunks for all three.
- **Model/Tables.h SceneTables.** About 80 parallel columns, each with its own rules: published or not, coordinator-only, which log replays it. Suggestion: group the columns into families, each with its own stamp and log, as `TableFamilies` already does.
- **Coordinator.h, class Coordinator.** It has 22 private bases and about 300 declarations, and almost every declaration had a doc block naming its callers and stages. The class is the whole subsystem. Suggestion: split it into objects with narrow interfaces (events and mirror, walk, joins, set and claims, publication), each with a short header doc. The current split into bases still shares one `this`.
- **Coordinator.h Stats / SetStats / TablesPublication / ResidentStats, and State/Pump.h, Attach.h, Materials.h counters.** Diagnostic counters dominate the interface, and many had a comment of several lines. Suggestion: one diagnostics struct per phase, documented once in the report code.
- **Coordinator.h RetiredBatch / retirement chain (with Tables.h RetiredKind, Publication.h retirementHold).** How long slots, imports and engine references live was explained only here.
  Original: "What the coordinator let go of that a publication may still name: slots (by index), imports (on the GPU), and engine references (nodes, materials, tracker events' detached subtrees, the candidate tables' entry roots). Each publication holds the chain node published with its tables (ScenePublication::retirementHold), and only the frame's path holds publications - the builder, the installed frame, the GPU executions, the readers that write to nodes later (readback write lists, fade stores, the cut's plans) - so a batch is returned as soon as the last frame that could read what it names is done: RecycleRetired frees the slots, hands the imports to the epochs and the references to EngineReleases (released at Present)." Doc: dclf-architecture.md, "Retirement".
- **Coordinator.h MaterialViewPins / Slots.cpp PinMaterialViews.** Records hold raw view pointers, and separate pins keep those views alive. Suggestion: store owning references in the record so it pins its own views.
  Original: "The record's views are raw pointers read from the engine's textures and the frame's sources, and the engine releases a texture on any thread (loader threads, ClearTextures on a cell's unload; Advanced Skin drops its entries on a load). The held capture the record was made from holds its views only until a newer capture replaces it or it ages out, while the record can stay longer (a rewrite the port cannot evaluate keeps it). ... A pointer read from the record is never referenced: it may be dead already."
- **Coordinator.h HiddenForWalk, State/Categories.h viewRoots, Classification.cpp ClassifyFrame, ApplyEvents.cpp DrainHiddenEvents, SceneWake.h.** The engine sets and undoes kHidden and alpha inside its render window, for its own views. Several places rely on this. Doc: skyrim-engine-notes.md.
  Original (HiddenForWalk): "ShadowSceneNode::OnVisible hides a portal graph's always-render children and shared node during the room traversal (main camera cull jobs); TESWaterReflections::Update hides the player's 3D while a cube-map reflection updates (Main::Draw, render thread, during the cull jobs); Main::Draw keeps the first-person skeleton hidden for every world view. Live bits would drop the player's face shapes on reflection-update frames."
  Original (SceneWake): "The hidden, leaf, value and property sources wake only outside the frame's render (the EngineReadWindow, Main::Draw to Present): inside it the engine sets and undoes them for its own views (GetRenderPasses leaves each camera's alpha on the property, the reflection faces hide the water, Main::Draw hides the first-person skeleton), and the frame's next pass takes them. What the frame's pass takes anyway does not wake it: the per-frame queues (moves, fades, node transforms and controllers, LOD fades, emittance, shading), object LOD's segment writes (the terrain manager refreshes them as the camera moves), node and geometry value updates, an attach out of the world (a loader's subtree: its world attach wakes), the material writers' captures (controllers write every frame), and the render thread's mid-frame posts."
- **Walk.cpp PerFrameOf / PerFrameTraitsOf / MoverDue / FrameVerdictKept, State/Tracked.h lightTraits, State/Placements.h.** Which entries are walked every frame, which take the "light path", and which are only placed is spread across comments in five places. The rules themselves are sensible (actors, skins, movers, ordered leaves, particle types). Suggestion: one table in dclf-architecture.md, "Movers by event", and fewer overlapping fields in Tracked (entryRoot, listedRoot, lightRoot, moveKey, moveKeyOuter).
- **Publication.h PlacementPlan + Placements.cpp.** The delta-set protocol between the coordinator and FrameValues (no plan may be skipped, reset plans, the parity check) is only in comments. Doc: dclf-architecture.md, "FrameValues".
- **AccumulatePhase.cpp / Residents.cpp / SceneSet.cpp rebind staging** (LeaveMainForRebind, StageBinding, RebindKeepsDrawing, LeaveSet). A member keeps drawing its old binding until the new one is ready. That spans the joins, the commit and ApplySet. Document it in one place, or simplify.
- **Coordinator.h Components\* hooks.** About 25 hooks, each doc stating exactly when it fires relative to the others. The ordering contract is real. Put it in one table in Api/CoordinatorComponents.h.
- **FrameView/FrameRequests.cpp FrameStartJob, CategoryWatch, category signature, unheld roots diff.** These are three overlapping change detectors for the category nodes. There is also a threading rule: engine reads may run on the pool inside the read window, but mirror captures must stay on the render thread, because other threads attach and detach inside the window. Suggestion: one detector, and document the window rule.
- **FrameView.h claims lifecycle** (InstallClaims, NotDrawingYet, AdoptedClaims, HandOverAtFrameStart). The frame-start ordering was spread across five doc blocks.
- **FrameView.h RefreshFrameConstants.** The Z-prepass epoch runs on last frame's constants. That is harmless only because World is patched per object and Terrain LOD's HighDetailRange is a technique row's. This is fragile coupling.
- **ScenePump.cpp PublicationDue / pacing, and the wake fence pairing with ScenePass.** Subtle, and explained only in comments.
- **Records.h ObjectFlags / PipelineRasterFlags.** Bits 20-21 mean decal group or terrain group depending on kObjectDecal. kObjectFocusOnly is ShadowOnly plus Decal by convention. Raster bits 21-23 mean three different things by key family. Suggestion: explicit accessors per family, or separate fields.
- **Records.h fade roots** (kFadeRootStoodIn, kFadeRootWritten, seed rows): who owns fade state (engine, FadeStateCS, write-back).
  Original kFadeRootStoodIn: "An owned root with no engine-drawn part, culled by the stand-in: its state is FadeStateCS's alone and its node takes the GPU's milestones and LOD levels from the fade write-back (FadeEvent; the animation gate skips it, Features/Fades' FadeWrittenByDclf). DCLF's readers take the GPU's state (BuildDrawsCS) or the settled one (ShadowCasterReject, the synthetic pass). An owned root with engine-drawn parts is culled by the engine; FadeStateCS runs the same update for its DCLF members (CS_DCLF_FADE_PARITY compares)."
- **Records.h CanonicalShadowTechnique, DrawSequence; Model MergeFadeSeed.** These duplicate HLSL rules and layouts (Utility.hlsl, DCLFObjects.hlsli, FadeStateCS.hlsl). The old comments with the full bit rules are in git history. Suggestion: generate them from, or static-check them against, the shader side.
- **Lookups.h Lookups.** There are five change signals (generation, shadowGeneration, versions, instance, ChangeKey), each with different readers. Document them once.
- **Eligibility.h OrderedLeaf, Sun(), Eligibility.cpp Cacheable/BufferVerdict/Layer.** These explain engine behaviour (ordered-node grouping FUN_1414b2330, 0x141503970, 0x140e28af0; multi-index shadow registration FUN_1414b2a60) and which verdicts may be cached, because Community Shaders' features set property flags in place. They belong in skyrim-engine-notes.md.
- **MaterialBindings.cpp / SharedBindings.cpp.** The handling of imports superseded by a move (pinless), and the amortized cache sweeps, are duplicated in the two files.
- **Internal.h ShardScope.** It lists which per-thread state a pool worker must rebind. It is easy to miss when someone adds a new per-thread input.

## 2. Hazards kept (index)

- Coordinator.h MaterialReference: AE material database release (manager 0x143187758, FUN_1414f7a40). A BSTSmartPointer would delete directly.
- Coordinator.h / Slots.cpp: never AddRef a view pointer read from a record. The engine releases textures on any thread.
- Coordinator.h ResolveSunEntry: the cascade cull FUN_140e305c0 walks only the entries the full-frustum cull FUN_141511f30 passed.
- Coordinator.h HiddenForWalk: the engine flips kHidden around its views, so FrameGlobals::cullHidden is used.
- Coordinator.h HiddenFocusCaster / Records.h kObjectFocusOnly: focus shadows register the target subtree whole, hidden parts included.
- Coordinator.h RefreshAncestorValue(s), Classification.cpp: actors moved between rooms keep stale LLF room values without a refresh.
- Coordinator.h HandBack, Internal.h BatchUnwindGuard, ApplyEvents.cpp: engine destructors must never run on the pump. References are released at Present.
- Slots.cpp ResetSlotTables: a material's last release goes to Present, since the pump runs this on a loading pass.
- Walk.cpp BuildScenePhase / FrameView.cpp NoteLoadingScreen: nothing may touch tracked geometry or walk the scene graph during a load (VK_ERROR_DEVICE_LOST).
- Walk.cpp NoteUnstable: DYNAMIC and CPU-access buffers are renamed by DXVK on discard maps and cannot be imported.
- Walk.cpp ResolveGeometrySlot and its siblings: engine draw shapes (FUN_1414f2ad0 types 1 and 8, FUN_140e465d0 hint 12).
- Walk.cpp record write: two-sidedness and alpha test are set in the scene phase, because shadow epochs read them before the accumulate phase.
- Walk.cpp skin palette: sized as AE FUN_140e4ff90 sizes it (also Internal.h SkinRowsOf).
- ScenePump.cpp: the wake fence pairs with ScenePass's take. Never throw out of the drain, or the pump closes for good.
- FrameView.h CatchUpSwitch (AE 0x140d29700) and FrameRequests.cpp: engine writes, render thread only.
- FrameView.h LatchAccumulator: currentAccumulator is null before the main pass.
- FrameView.h TakeRetiredImports: in-flight GPU reads by device address are invisible to DXVK.
- FrameView/FrameConstants.cpp: the eye is the main camera's only from Prepass. ProjectedUV textures come from BSGraphics::State (SE 0x14302c890, AE 0x14328cc20).
- FrameRequests.cpp: mirror captures stay on the render thread, since other threads attach and detach inside the read window. The particle manager frees masters below count 3. BSLightingShader instance 0x14338ca00.
- Coordinator/FrameConstants.cpp: BSIntrusiveRefCounted +0x8 (FUN_1414ac820).
- Classification.cpp: particles under billboards need no turn (0x140d3e650, UpdateWorldData 0x140dd4c90). The first-person root's kHidden is view-scoped (FUN_140736150).
- ApplyEvents.cpp / SceneMirror.h / Publication.h: RaceMenu writes overlay alpha and skin partitions with no hook, and overlays share their base's skin instance.
- SceneMirror.cpp: lights and cameras leave through ShadowSceneNode's queued removal, with no hook.
- ApplyEvents.cpp: the engine moves objects by detach plus attach. A load pushes thousands of events a frame.
- Tables.h shadowDiffuse: BSLightingShaderPropertyFloatController moves the texture transform between Main::Draw and BeforeShadowMaps.
- Tables.h / MaterialBindings.cpp: membership in a used set certifies raw engine pointers.
- Records.h kObjectNoBindings: the tables may be empty, so even index 0 is unsafe.
- Records.h FadeFrame: AE fade global addresses (trailing comments kept). Process1 flags (AE 0x140e28390).
- Records.h DrawSequence / MergeFadeSeed / CanonicalShadowTechnique: must match the HLSL.
- Lookups.h SharedChunks: sharing is decided by stamp, never by a reference count another thread moves. A Lookups reset restarts generations, so key on `instance` too.
- SceneIdentity.h: fail closed before an ID wraps (ABA).
- Channels.h CategoryCapture: unheld root pointers are keys only, readable while pins hold them.
- State/Mirror.h, State/Dependents.h, Classification.cpp: never make a reference from a key.
- State/Slots.h: leases hold the old storage after a DXVK buffer move.
- State/Components.h: root marks are written only while events apply, never during parallel prepares.
- State/Tracked.h hiddenChain: owed to the per-frame walk, or the first-person skeleton hide is lost.
- MaterialBindings.cpp / SharedBindings.cpp: an owner's last release runs on the cleanup queue, never on the scene lane.
- Residents.cpp: a leaf update's property swap rewrites no slot, so it must note the slot for its pipeline template recount.
- AccumulatePhase.cpp: the Lighting shader is known to material sources only from the frame after it was found.

## 3. Possible bugs or dead code noticed (not fixed)

- Walk.cpp BuildScenePhase had "geometry and pipeline slots keep their grace period", but Slots.cpp UpdateSlotReferences says geometry slots are freed "with no grace period". One of the two is stale. I removed the Walk.cpp line; check which is true.
- Coordinator.h ResidentStats::failedBy[0] is documented as unused.
- Tables.h kChangeRetired2: a retired change bit, kept only for report naming.
- State/Walk.h PerFrameTrait bit 2 is unused.
- State/Dependents.h rootDependentsAt / lightDependentsAt / hiddenDependentsAt: the comment said they are unused until those lists' insertions go through List (it cited a planning doc, requests-w1.md R1/R2). Check whether they are dead.
- State/Materials.h watchesSkipped / watchesDropped: the comment said "no longer moved (the transform watch is gone)", so these are dead counters.
- Model/Publication.h: the ScenePublication doc block was orphaned above PublishedScene in HEAD. I moved a one-line version to its struct.

## 4. TODO/FIXME lines found

None.

## 5. Coverage

- All 67 files in the slice are done: Coordinator/ (cpp, h, State/), FrameView/, Model/, and the top-level headers. None are left.
- Comment lines (lines starting with `//`, `/*` or `*`, so the count includes a few `*a_out = ...` code lines): **4112 before, 1681 after**. The diff is 1447 insertions and 3878 deletions, all on comment lines.
- Checked mechanically: with comments and whitespace stripped, every file matches HEAD. No added line is code. A scan found no broken comment fragments.
- Whitespace: replacement comments are indented to match the code that follows them. Trailing comments on code lines were mostly left as they were.
