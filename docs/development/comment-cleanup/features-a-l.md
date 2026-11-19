# Comment cleanup notes: features-a-l

Slice: the sub-folders of `src/Features/DrawcallLimitFix/Features/` from `Actors` to `Lod`, in the test-runner worktree.
The work was split into four parallel sub-slices with disjoint folders. Each section below has one part per sub-slice.

Slice total: 3,518 comment lines before, 1,596 after (`grep -E '^\s*(//|/\*|\*)'`). 138 files changed, all of them pass
a check that compares each file with HEAD after stripping comments and whitespace.

## 1. Simplification or documentation targets

### Fades

- **`FadeCpuStage.cpp`, file header, and `FadeScene.h` `RegisterCpuStage`.** A 12-line header was the only description of the CPU
  version of FadeStateCS. It covered its frame-value pool, its kick and sample points, the portal-program mirror, and a deliberate
  difference: without a program the camera test uses the engine's culling planes, not FadeStateCS's view-projection planes, so the
  two differ by rounding. Two implementations of one step (HLSL and C++) plus the engine's own makes three sources of truth.
  Suggest one doc section on "fade step: GPU, CPU and engine" in dclf-architecture.md, or share one plane source between them.
  Original, kept for reference: "States are this frame's, not the frame before's (FadeStateCS publishes a frame ahead): the stage
  runs before the frame's culls. The camera test without a program is the engine's (Process1's sphere against the camera's culling
  planes, ShadowCameras::FrustumPlanes), not FadeStateCS's view-projection planes: the two agree but for rounding at the planes."
- **`FadeWriters.h` animated batch drain.** The comment was the only proof that no writer pushes between the frame start and the
  producer's drain. It relies on the cell animations job pushing only after sampling, and Main::Update's jobs waiting for the drain.
  That is an ordering argument across three threads that the code does not show. Suggest a graph requirement or an explicit
  sequence number on the batch, so the proof is not needed. Original: "Nothing pushes between the frame's start and the producer's
  drain: the cell animations job pushes inside Main::Draw only after the frame's sampling, which the drain is part of
  (WindowWriters: FrameValues::WaitSampled), and Main::Update's jobs push after its entry, which waits for the drain
  (WaitAnimatedDrain). So the producer's drain takes the batch the frame start's took."
- **`FadeWriters.h` writer tables slot check.** Animation-job updates resolve their root through tables pinned at Main::Update's
  entry, and a consumer reading a different table set must recheck node and generation. Two table generations live at once.
  Suggest documenting the writer-table lifetime in dclf-architecture.md next to the scene tables' swap.
- **`FadesInternal.h` actor fadeAmount reseed and `FadeEvents.cpp` fade events.** Actor::UpdateAlpha ramps fadeAmount after a cell
  load, and a root listed mid-ramp keeps a partial alpha unless the seeds' frame start rereads it. The fix is a third path next to
  the SetAlpha events and the placement snaps. Suggest one "root reseed" entry point with a list of its triggers, and an engine-notes
  entry for the SetAlpha and FUN_14147aa20 writers.
- **`FadePixelRows.cpp` `PixelRowsPass`.** The pass declares the pool as written, then still adds full memory barriers before and
  after for guest-mode epochs, because the pool's backing is reused across frames. The graph's declarations alone do not order it.
  Suggest the graph track reuse of a pool backing across frames, so the manual barriers can go.
- **`FadeState.h` port.** The port must match the engine instruction for instruction, including MINSS/MAXSS and unordered
  comparisons, and the static row carries lodScale from the CPU CRT because logf/powf differ on the GPU. Kept as a short header.
  Suggest a section in skyrim-engine-notes.md listing the engine functions and the bit-exact rules.

### Lod, Actors, Billboards, BloodSplatter

- **`Lod/LodGates.h`, the namespace overview, and `Lod/LodGates.cpp` (Gate, Handoff, VetoTable, FlushAll, Retire, OnAttach).**
  A 25-line block was the only description of the LOD gate lifecycle. A swap hides the incoming blocks before they attach. It steals the
  outgoing blocks' handles inside FUN_140510a10, so the engine skips their detach and release. The gate is pushed as a stacked scene
  event. The coordinator flips it once the incoming geometries are ready, the render thread calls Flip, and the next LOD drain releases the
  stolen blocks through the engine's own functions. On top of that, three lock-free races exist: gate state token-word CAS, a per-block
  `handoff` CAS between reattach and flip, and a reader-counted veto table. Forced releases have 9 sources and supersede, orphans, kept
  notices and deferred releases are extra paths. None of this can be followed from the code alone.
  Suggestion: write a doc section with a state diagram (gate states and block handoff states) and the thread that owns each transition.
  Better still, simplify. Forcing every engine-thread transition under the manager's lock and letting Flip only *request* a flip (applied
  by the next drain under the lock) would remove the Flipping state, flipActive and the handoff CAS. The cost is one drain of latency on
  the outgoing hide. The engine-side reference is skyrim-engine-notes.md, "LOD swaps and their gates".
- **`Lod/LodGateComponent.h/.cpp` (OpenGate, TagGate, RetireGate, ApplyGateKept, Settle).** The scene side spans marks owned by the
  coordinator, per-geometry tags owned by the component, Observe/Hold vetoes owned by the coordinator, requeue flags and deltas.
  The comments explained which of these owns what at each site. Example of the original (TagGate): "Withheld while its gate is open, its
  slots' readiness recorded (Observe: ready only by an evaluation under this gate, an earlier gate's record of the slot is not this one's);
  a hold-out it had goes. Classified again with its root gated (a verdict taken while the gate's hide counted stands no more; AddGeometry's
  own is pending already), and its slots committed again by the next commit." Suggestion: dclf-architecture.md "LOD gates: the scene side"
  should give the per-geometry lifecycle (untagged, incoming-observed, outgoing, held-out, kept) as a table, with which hook moves it.
- **`Lod/Diagnostics/LodGateParity.cpp`, CheckOpen's "counted" rule.** A five-line comment explained when the pending count is allowed
  to differ from a fresh count (deferred gates, retags in the settle's walks, nested roots). Original: "The count: not for a gate the
  structure check deferred (every geometry ready by the commit's records; pending then counts the records not published yet), nor after a
  tag in the settle's walks (an unmirrored gate's walk may take a geometry of a gate counted before it; each such tag of a geometry with
  slots requeues them, and the requeued slots are none after the commit's start otherwise). A geometry with neither slots nor a verdict
  queues nothing: taking it needs a root nested under another gate's, which the marks (G6) flag." The invariants G1-G7 are only named in
  this file. Suggestion: list G1-G7 in the architecture doc next to the scene-side lifecycle. Consider making Settle's walks not retag
  across gates so the exception goes away.
- **`Actors/ActorRate.cpp` (file header, UpdateActorRate).** DCLF writes the engine's per-frame visibility stamp
  (`BSShaderProperty::lastAccumulatedFrameCount` on the MiddleHigh skin property) so off-screen actors that cast shadows keep animating at
  full rate. The engine sets this stamp only in its depth and shadow registrations, which DCLF replaces. This is a surprising coupling
  between DCLF's culling and actor animation. It is kept as a 6-line header. Suggestion: document it in skyrim-engine-notes.md (already
  referenced) and in the architecture doc's list of "engine side effects of the culls DCLF replaces".
- **`BloodSplatter/BloodSplatterDraws.cpp` (Client::gate, runs, target, the format-change path in CanTake).** Per-feature comments explain
  shared render-graph anchor machinery: run gates closed every run and opened only by the owning client's commit, RunSlots because
  record-at-execution may happen on the submission thread after the next run is published, and swap-chain format changes making a new
  slot. Every engine draw family probably repeats this. Suggestion: document it once in the render graph docs (FrameAnchors clients), or
  wrap it in a FrameAnchors helper so families do not hold gate, runs and target themselves.
- **`Lod/LodFeature.cpp` and the other Install functions (registration order).** Many removed comments recorded where Core used to list
  each switch, toggle and hook ("where Core's were listed..."). The order is now expressed by `.before`/`.after` fields, but some ordering
  still relies on feature registration order alone. One example is the gates' component running before the segments' and every later
  feature's. I kept one line for that. Suggestion: make component order explicit too, so nothing relies on FeatureList order.

### Grass, Effects, Faces

- **Faces/FaceSnapshots.h, class FaceSnapshots.** The 31-line class comment was the only full explanation of face snapshots.
  Two engine hooks (after each head's morph job at 0x1404334f3, and after the stage join at 0x1406d36fd) copy positions into a
  per-head triple buffer. Records are created only by the scene walk, retired when the head's last shape leaves, and freed only
  after a later morph stage completes. A publication tells the coordinator which heads to update in place or rewrite. None of
  this lifetime and threading contract shows in the code, which spreads it over hooks, an open-addressed table, leases and
  tombstones. Suggestion: move the full contract into dclf-architecture.md, "NPC faces". Also consider collapsing the
  record-lifetime rules (walk-only create, retire, free after a stage) into one small type that owns the rule.
  Kept: 7 lines covering the hooks, triple buffer and free-after-stage rule.

- **Grass/GrassMirror.h, class Mirror (and GrassMirror.cpp Stale/RemoveShape).** Event ordering relies on per-group seqs,
  shape and group tombstones kept for kTombstoneDrains, and a rule that a shape removal takes only groups older than it,
  because the capture queue drains its ring before its spill and so reorders. The comment carried the whole reasoning.
  Suggestion: make the queue deliver in seq order, or move ordering into the queue, so the mirror no longer needs tombstones.
  Otherwise document the ordering contract in dclf-architecture.md, "Grass: the draws".

- **Grass/GrassDraws.cpp Owned(), Grass/Grass.h top.** When DCLF owns grass the engine's grass passes are withheld, so
  Grass Optimizations and Grass Collision must stand down on exactly those frames, and Grass Collision's update runs from DCLF
  instead. The load hold also withholds grass before DCLF owns it, so stand-down follows `owned || LoadHold::Active()`.
  This cross-mod handshake is invisible from either side's code. Suggestion: give GrassShared one explicit "grass frame owner"
  enum that both PassCapture's withholding and the stand-down read, so the two cannot disagree. Document it in the architecture doc.

- **Effects/EffectDescriptors.cpp EffectDrawOf / EffectVariantsOf.** About 40 lines of comments mapped engine render hints
  (0 opaque, 1 blended, 2 and 3 decals), Falloff without blending, opaque effects through decal group 1, rigid skins and the
  cloud light onto DCLF groups. The branching depends on reverse-engineered engine behaviour that the code cannot show.
  Suggestion: a table in dclf-architecture.md, "The Effect shader in the forward epoch", with one row per engine hint and flag
  combination and the DCLF group it maps to. Then the code could follow the table with one-line comments.

- **Effects/EffectWrites.cpp EffectControllerUpdate.** Which material bytes belong to the frame (patched every frame by
  TextureTransforms and the port's frame variables) and which to the record decides whether a controller write recaptures the
  material. The comment named three systems to explain it. Suggestion: have the frame-variable registry expose a single
  "frame-owned byte range" query so the rule lives in one place.

- **Grass/GrassTables.cpp Require.** Table floors exist because shrinking in an interior made the first exterior commits
  unable to fit grass, so none drew while the engine's passes were withheld. A history-driven rule. Suggestion: the scene
  buffer set could keep a high-water floor per table generally, instead of each feature remembering this.

### DecalOcclusion, Decals, FirstPerson, Forward, GpuOcclusion, HiddenNodes, Landscape, LocalLights

- **FirstPerson/FirstPerson.h (file header).** The 28-line header listed every piece of the first-person view and said which part is the feature's and which is Core's: epoch segments, slot state, set decisions, placements, FrameValues palette rows, PassCapture withholding. A bucketed view's behaviour is spread over about ten Core files keyed by slot, and this header was the only map. Suggestion: add an "Anatomy of a bucketed view" section to dclf-architecture.md, or gather the per-view Core pieces behind one ViewSlot object so the type itself is the map. The original text is in HEAD.
- **FirstPerson/FirstPersonView.cpp FirstPersonLatch.** This recomputes Main::Draw's cVar23 by hand: the root is not hidden, the main menu is not open, and the camera is not free. Duplicated engine logic only stays correct if someone re-checks the decompile. Suggestion: describe it in skyrim-engine-notes.md, "The first-person view", which the code already references.
- **Forward/ForwardHooks.cpp MarkRefractionNormals, Main_Draw_RefractionCheck, and Forward.h PreflightHooks.** DCLF draws the refraction normals early. It then sets two engine bytes (0x1435ef130/131, begun and drawn) so the engine's later draws add to the normals instead of clearing them. Because "begun" is set, it must also clear accumulator group 6 at Main::Draw's refraction point, where the engine would otherwise draw it, which it never does normally. All three hooks have to install together or not at all. This is a protocol between hooks, built on engine globals, and only the comments explained it. Suggestion: document the refraction normals' lifecycle in skyrim-engine-notes.md, and install the three hooks as one unit that refuses a partial install.
- **Forward/Forward.h (file header).** 13 lines on what Core owns and what the feature owns in the forward epoch: ranges, sort, shared epoch submission, refraction pipeline, normals import, DeriveForward. Suggestion: put this ownership table in dclf-architecture.md, "The forward epoch".
- **LocalLights/LightSelection.h (LightSelection class doc).** A 30-line doxygen block was the only description of the CalculateActiveShadowCasterLights port. It covered what DCLF reads and when, and how its room test differs from the engine's: DCLF uses this frame's portal walk, while the engine uses each light's last walk. It also covered the focus-shadow host rules with their flag that persists across frames, and who reads the result. That is a port of engine logic plus a deliberate difference from it. Suggestion: a "Shadow light selection" section in skyrim-engine-notes.md, with the room-test difference called out. The original is in HEAD.
- **LocalLights/LightSelection.cpp Select (focus-host loop).** This copies an odd engine loop on purpose: `focusFound = light == lastFocusHost && !focusFound` runs over every candidate, and the last one wins. Suggestion: record this in the engine notes so nobody "fixes" it.
- **LocalLights/LocalShadowLights.h.** The doc explained how GetRenderPasses picks an object's shadow lights: FUN_1414fcf80, activeLightMask, the landscape +0x61 rule, and Light Limit Fix's ShadowBitMask built from maskIndex. It was the only explanation of the receiver mask. Suggestion: move it to the engine notes ("light selection").
- **LocalLights/LightContextResources.cpp.** It stacks several caching layers, each keyed by a hand-mixed hash (rootsKey, columnKey): late-bound VersionedBuffer publications, a versioned object-gate column, a static `sent` mirror for delta uploads, and gate interning with a reset cap. Suggestion: one cached "gate table" type with an explicit invalidation input, in place of the ad hoc key mixing.
- **Decals/DecalOrder.cpp (OrderDecals, RekeyChanged) and DecalOrder.h OrderKey.** This code reproduces the engine's decal draw order. Registration runs in reverse. BSOrderedNode children come after plain appends. Decal nodes visit their decals last to first. Keys come from mirrored parent chains, and siblings under a decal node are re-keyed incrementally. A parity rebuild catches stale keys. The comments were the only statement of these ordering rules. Suggestion: keep skyrim-engine-notes.md "The main registration's order" as the authority and link to it. Also consider always doing a full rebuild, since the parity check already does one every 60 passes, unless the incremental path is shown to be needed.
- **Decals/Decals.h (file header).** A 13-line map of what the feature owns and what Core owns: decal groups, raster keys, ordinal columns, sequence buffer slots. Suggestion: move it to dclf-architecture.md, "Decals and pass layers".
- **DecalOcclusion/DecalOcclusion.h (file header).** 20 lines were the only end-to-end description of how DCLF takes over the engine's decal occlusion test. The engine's query calls are bracketed so they go nowhere, and DrawTriShape is latched instead of drawn. The graph redraws the shapes and answers the engine's ring slot with the summed count. A skipped test answers 1, and passes not drawn through DrawTriShape still reach D3D11. The mechanism crosses Queries, Family and FrameAnchors. I cut it to about 10 lines. Suggestion: make sure docs/development/p5/occlusion-queries.md holds the full text (it is in HEAD).
- **DecalOcclusion/DecalOcclusionDraws.cpp.** The code computes several things by hand (`InstanceEntry`, `instance * kMaxDraws`): instances per start-stage run (kInstances), the mapping from tap index to instance, gate entries, latch regions and query sub-ranges. Suggestion: a small per-instance slice helper type, so the layout lives in code instead of comments.

## 2. Hazards kept

### Fades

- FadeState.h: the port must match the engine bit for bit, including MINSS/MAXSS order and unordered compares. lodScale comes from the CPU CRT.
- FadeCpuStage.cpp: CPU stage states are this frame's, the GPU's are a frame ahead. The no-program camera test uses the engine's planes.
- FadeWriters.h: nothing pushes animated updates between frame start and the producer's drain (ordering argument kept short).
- FadeWriters.h: writer tables are pinned at Main::Update's entry. Readers of other tables must check node and generation.
- FadePixelRows.cpp: full barriers before and after, because the pool backing is reused across frames in guest mode.
- FadeRecords.h: FadeEvent generation guards against a slot reused since the event was written.
- FadesAddresses.h: SE indexes two threshold arrays by node kind (1 fade node, 4 tree). AE reads four floats.
- FadesAddresses.h: the tree height-test switch is a byte field on the culling process (+0x121), not a global.
- FadesFeature.cpp: RootGlobalsKey must mix in the order MembershipWitness used.
- FadesFeature.cpp and FadeRender.cpp: install order. Family after TreeWind's, component last, writers last, seeds the last frame value pool, resources and commit after every other feature's.
- Fades.h: the feature must stay last in FeatureList.cpp.
- FadeEvents.cpp: placement snaps (FUN_14147aa20) and Actor::SetAlpha (vfunc 0xE1, +0x100) write fade state behind DCLF's back.

### Lod, Actors, Billboards, BloodSplatter

- `Lod/LodGates.cpp`: engine layouts of quadtree node, handle and blocks, the same on AE and SE.
- `Lod/LodGates.cpp`: SE inlines FUN_14050f790, so the full-rebuild hook sits on its first call (FullRebuildSE).
- `Lod/LodGates.cpp`: SE's handle release needs the block cache (+0xD0), and there is no handle-only form.
- `Lod/LodGates.cpp`: the memory context 0x55 must be set so the engine's allocation accounting accepts our releases.
- `Lod/LodGates.cpp`: the terrain manager's recursive lock guards all engine-side gate state, and DCLF adds no lock.
- `Lod/LodGates.cpp`: gates are never freed, and the state word carries the token, so stale exchanges fail.
- `Lod/LodGates.cpp`: flipActive is set before the exchange, which is how FlushAll knows no flip is writing.
- `Lod/LodGates.cpp`: veto table slots are written once and old tables are freed only when vetoReaders reaches 0.
- `Lod/LodGates.cpp`: FUN_14050e430's early return skips the drain, so the flush happens there.
- `Lod/LodGates.cpp`: only the full-rebuild call at 0x14050e7ae is hooked, because the round-robin update also passes 1.
- `Lod/LodGates.cpp`: InitialLoad under a load screen passes through, because the load screen ends in a full rebuild.
- `Lod/LodGates.cpp`: FUN_140509550 attaches only a loaded terrain block, so the pre-hide is undone when it did nothing.
- `Lod/LodGates.cpp`: FUN_140512160 reads the cell's worldspace, which is null in an interior.
- `Lod/LodGates.h`: a detour that fails to attach forces DCLF off, because only the detours return stolen handles.
- `Lod/LodGateComponent.h`: kNoSlot must match Core's kNoObjectSlot.
- `Lod/LodGateComponent.cpp`: Take runs before the event stack drains, and ApplyOutcomes after the batch. That ordering is what puts events before outcomes.
- `Lod/LodGateComponent.cpp`: an open gate with no incoming roots never flips, and only a forced release retires it.
- `Lod/LodGateComponent.cpp`: a kept notice can arrive before or after its gate's flip outcome.
- `Lod/LodSegments.h`: segment layout and engine behaviour (FUN_140e31160/FUN_140e31130, dirty byte, run rebuild in FUN_140e31180).
- `Lod/LodSegmentHooks.cpp`: segment writes do not wake a pass, on purpose. They are constant and mostly no-ops.
- `Lod/Diagnostics/LodSegmentParity.cpp`: one engine-read lease per shape, since scene work can run beside the engine's update.
- `Lod/Diagnostics/LodGateParity.cpp`: gate roots are keys only and are never dereferenced.
- `Actors/ActorRate.cpp`: the animation-rate stamp (0x1406a37ef) and why DCLF must write it. Main thread, no jobs running.
- `Actors/ActorEvents.cpp`: Actor::SetAlpha vfunc 0xE1 writes fadeAmount at +0x100 on both 3D roots.
- `Actors/Actors.h`: NeedFadeAmounts must run at install, before the early hooks fire.
- `Billboards/BillboardOrient.cpp`: the engine's float constants copied bit for bit, and the summation order kept.
- `Billboards/Billboards.h`: Orient must match Effect.hlsl's DCLFBillboardRotation.
- `Billboards/BillboardsFeature.cpp`: the parity observer must hook before the primary cull takes NiBillboardNode's OnVisible slot.
- `Billboards/BillboardsFeature.cpp`: particle systems under billboards are current because 0x140d3e650 runs UpdateDownwardPass before ParticleSnapshots.
- `Billboards/Diagnostics/BillboardTurns.cpp`: we multiply in a different order from the engine, so ulps differences are expected.
- `BloodSplatter/*`: volk must precede every Vulkan header (kept as is).
- `BloodSplatter/BloodSplatter.h`: DrawConstants must match BloodSplatterOrgDraw in the HLSL (static_assert as well).
- `BloodSplatter/BloodSplatterFeature.cpp`: Reserve must run before the render graph initializes.
- `BloodSplatter/BloodSplatterDraws.cpp`: the run gate stops another client's run of the shared anchor from redrawing this client's old run.

### Grass, Effects, Faces

- Grass/GrassTables.cpp BindBucket: diffuse must sample clamped, or wrap bleeds opposite-edge stems into card corners past the alpha test.
- Grass/GrassTables.cpp WindTimeBase: timer at 0x142033090 scaled to 1/600 turn, as BSGrassShader::SetupGeometry.
- Grass/GrassTables.cpp Require: never shrink below first capacity (interior-to-exterior hole).
- Grass/GrassTables.cpp Commit: a commit that cannot draw while passes are withheld leaves a visible hole.
- Grass/GrassDraws.cpp Owned: load hold withholds grass before ownership; use the camera, not posAdjust, for the eye.
- Grass/GrassDraws.cpp Decide: a rejected group is engine-drawn, so grass cannot be withheld while one lives.
- Grass/GrassDraws.cpp / GrassLayout.h DrawArgs: Vulkan skips the instance word via argumentsOffset; D3D12 writes it to a push constant.
- Grass/GrassLayout.h: shaders mirror the structs byte for byte.
- Grass/GrassMirror.h Event: shape pointer is a key only, never dereferenced; Remove fires for all BSMultiStreamInstanceTriShapes.
- Grass/GrassMirror.cpp: LOD mesh record must be held (activeBuckets) or the shared library drops it.
- Grass/GrassHooks.cpp: lodMesh read while the shape is alive; event fields live only during the call.
- Grass/GrassPasses.cpp: colour arguments pass always runs because it zeroes next frame's counts.
- Effects/EffectMaterial.cpp: engine dereferences the greyscale palette without a null check.
- Effects/EffectMaterial.cpp: Effect port must register first to get material kind 1.
- Effects/EffectMaterial.cpp: soft depth stored raw so the record does not change with the camera.
- Effects/EffectDescriptors.cpp EffectSkinned: use the geometry's own vertex desc (+0x148), not the renderer data's.
- Effects/EffectDescriptors.cpp: kTempRefraction with kDynamicDecal registers no engine pass.
- Effects/EffectDescriptors.cpp: Lighting bit also gated by global 0x142033508 (assumed 1).
- Effects/EffectConstants.h: record bit 3 and bits 4-7 shared with refraction.
- Effects/EffectsFeature.cpp: write hooks must install after Core's Lighting write hooks; LLF input needed by name.
- Effects/EffectFrame.cpp: DLightColor multiplied in the engine's mulss order.
- Faces/FaceSnapshots.h/.cpp: both hooks or neither; retired records freed only after a later stage; engine refs released off the scene work.
- Faces/FaceSnapshots.cpp Insert: record goes in before the key.
- Faces/FaceShapes.cpp FaceRegionOf: retire, not free, because installed streams may still name the region.

### DecalOcclusion, Decals, FirstPerson, Forward, GpuOcclusion, HiddenNodes, Landscape, LocalLights

- FirstPerson/FirstPersonView.cpp: the engine moves the first-person root to the origin around its draws (FUN_1414ce9d0), so DCLF reads rows at the frame's start.
- FirstPerson/FirstPersonView.cpp Covered: the forward part has no plain buckets. A GPU-driven graph without device-generated commands must use the direct forward family, or leave the view to the engine.
- FirstPerson/FirstPersonEpochs.cpp CaptureCamera: the engine's first-person camera translation is in root space. The eye comes from DrawWorld +0x00, which is world space.
- FirstPerson/FirstPersonFeature.cpp: frustumCull=false because the frustum test dropped the weapon (its skinned bound).
- FirstPerson/FirstPerson.h, Forward/Forward.h PreflightHooks: must run before any hook is written. The sites are CALLs, so the thunks chain.
- Forward/Forward.h: install all three forward hooks or none, because the refraction point undoes what the blended draw marks.
- Forward/ForwardHooks.cpp: the blended-draw function serves every render-mode-0 accumulator. Only the main one triggers the epoch.
- Forward/ForwardHooks.cpp: in native mode the blended draw is the deferred+water commit point every world frame, even when the forward epoch is off. Under water, the water draws after it.
- Forward/ForwardPasses.cpp RefractionDrawPass: the D3D12 layout holds only root constants, and blocks are reached through push words. The D3D12 signature reads the instance word just before the arguments.
- Forward/ForwardEpoch.cpp ForwardEpoch: the first call must come after InstallFeatures has resolved the schedule (static init).
- LocalLights/LightSelection.cpp Visible: each light class sums squared distance in its own order, to match the engine's float results.
- LocalLights/LightSelection.cpp Select: the engine leaves the extra accumulator slots (per hemisphere or cascade) stale. They are null in `slots`.
- LocalLights/LightSelection.cpp Select: the per-light maps are pruned because a freed light's address can be reused.
- LocalLights/LightSelection.cpp Select: the atlas encode runs inside the engine detour, so a failure must not throw.
- LocalLights/LightSelection.h FrameLight: the engine can select twice per frame, and the first call's lights still draw.
- LocalLights/LightContextResources.cpp Commit: commit hooks run unguarded on the render thread. A failed publication publishes nothing.
- LocalLights/LightContextResources.cpp Gates::published: the CPU pipeline's culls read the column off the render thread, hence the immutable copy.
- LocalLights/LightContextResources.h: readers must hold the publication while they use its descriptors.
- Decals/DecalOrder.cpp: within a chain, a later registration draws earlier, because RegisterPass prepends.
- Decals/DecalOrder.cpp IsDecalNode: the exact RTTI match is intended, because OnVisible's behaviour matters, not the base class.
- Decals/DecalOrder.h OrderedDecal: places under a decal node count from the array's end. An edit shifts every sibling, so they are re-keyed together.
- Decals/DecalHooks.cpp: the array-edit hook runs after the call. The capture runs inside the engine-read window.
- DecalOcclusion/DecalOcclusion.h: a skipped test answers 1, so the decal is placed. Passes not drawn through DrawTriShape still reach D3D11.
- DecalOcclusion/DecalOcclusionDraws.cpp: the render area must be clamped to the target. Every instance is declared, because the engine decides late how many tests it runs.
- DecalOcclusion/DecalOcclusionCapture.cpp: another plugin's hook may stop the engine from beginning its query.
- DecalOcclusion/DecalOcclusionFeature.cpp: the anchor must be reserved before the render graph initializes.
- GpuOcclusion/HzbPass.cpp: on AE the HZB is built before the first-person depth and room stencil draws, so it has fewer occluders, never more. HZB padding holds the far plane, so it can only prevent culling. SPD avoids a full barrier per level.
- HiddenNodes/HiddenStores.cpp: a vetoed show store does not run and sends no event, so the object stays hidden. The veto cells must be set before the first event.
- HiddenNodes/HiddenNodesFeature.cpp: the stores must install in the early hooks, because the kept scene lists and the LOD gates check HiddenStoresLive when they install.
- Landscape/LandscapeSeams.cpp: the seam resources are IMMUTABLE D3D11 resources or file textures, exempt from graph tracking. Null descriptors have device owners, not imports.

## 3. Possible bugs or dead code noticed

### Fades

- FadeLayout.h `FadeStateConstants::reservedProgramsWord`: unused, the programs come from the depth latch. Could be dropped if the HLSL layout is changed too.
- FadeLayout.h: the old comment named "the parity log's first root" as a constant, but no such field exists in the struct. The comment was stale.

### Lod, Actors, Billboards, BloodSplatter

- `Lod/Diagnostics/LodGateParity.cpp`: the old header said the sweep runs "every 60th pass", but OnCommitted calls
  `ParityDue(passSerial, 37)`. The comment was stale. I dropped the number. Check which one is intended.
- `Lod/LodGates.cpp`, log strings still carry the piece name "T6b5", and `Actors/ActorRate.cpp` and `Billboards/BillboardsFeature.cpp`
  carry "T7f". These are string literals, so I left them unchanged.

### Grass, Effects, Faces

- Grass/GrassTables.cpp Setup: `if (auto* host = RenderGraphRuntime::Get().Host())` declares `host` but never uses it beyond the test.
- Effects/EffectDescriptors.h: enum value kEffectVariantBlood exists but EffectVariantsOf never sets it (kWeaponBlood is ported). Possibly dead.
- Grass/GrassPasses.cpp Revision functions are empty; fine if intended, but both cull and args passes rely on the revision elsewhere.

### DecalOcclusion, Decals, FirstPerson, Forward, GpuOcclusion, HiddenNodes, Landscape, LocalLights

- LocalLights/LightContextResources.cpp:UploadColumn: `static std::vector<std::uint32_t> sent` is shared across Scene instances. With two scenes, or a scene rebuild where FilledContents still matches, the delta could compare against another scene's words. It is only safe if one Scene exists at a time.
- DecalOcclusion/DecalOcclusionDraws.cpp (commit lambda in MakeClient): `instance = min(TapInstance(), kInstances - 1)` clamps a fifth or later test onto instance 3, which overwrites that instance's run slot and latch entries. The old comment called a test past kInstances "a tap error", but the code clamps silently.

## 4. TODO/FIXME lines found

### Fades

None.

### Lod, Actors, Billboards, BloodSplatter

None.

### Grass, Effects, Faces

None.

### DecalOcclusion, Decals, FirstPerson, Forward, GpuOcclusion, HiddenNodes, Landscape, LocalLights

None.

## 5. Coverage

### Fades

All 30 files done (FadePortalBatch.cpp had no comments). Comment lines 808 before, 467 after.

### Lod, Actors, Billboards, BloodSplatter

All 30 files in the four folders are done: every `.cpp`/`.h` under Lod (17, including Diagnostics), Actors (4), Billboards (5, including Diagnostics), BloodSplatter (4).
Each file was checked mechanically: code with comments and whitespace removed is identical to HEAD.
Comment lines (`^\s*(//|/\*|\*)`): **827 before, 380 after**. `git diff -w --shortstat`: 30 files, 403 insertions, 851 deletions.

### Grass, Effects, Faces

All 32 files under Grass, Effects and Faces (including Diagnostics) done. Comment lines (grep `^\s*(//|/\*|\*)`): 951 before,
387 after. `git diff -w --stat`: 32 files, 428 insertions, 993 deletions. Every file passed a mechanical check that the code
with comments and whitespace removed is identical to HEAD.

### DecalOcclusion, Decals, FirstPerson, Forward, GpuOcclusion, HiddenNodes, Landscape, LocalLights

- Done (47 files, every .cpp/.h under the eight folders): DecalOcclusion (4), Decals (6), FirstPerson (8), Forward (5), GpuOcclusion (4), HiddenNodes (5), Landscape (5), LocalLights (10).
- Not done: none.
- Comment lines (`grep -E '^\s*(//|/\*|\*)'`) went from 932 to 362. `git diff -w --stat` gives 400 insertions and 972 deletions.
- The mechanical check (comments and whitespace stripped, compared with HEAD) passes for all 47 files.
