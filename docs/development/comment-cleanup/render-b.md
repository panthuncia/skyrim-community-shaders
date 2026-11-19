# render-b comment cleanup notes

Slice: Core/Render/{Payload,Readback,Resources,Revision,Shadow,Snapshot,Views}. Done in four disjoint parts; each part keeps the five required sections.

Totals: all 64 files done. Comment lines 3,860 before, 2,130 after (by the verify.py count of full comment lines). Every file matches HEAD with comments and whitespace removed.

## render-b part 1: Core/Render/Resources and Core/Render/Readback

### 1. Simplification or documentation targets

- **Resources/GpuLayouts.h, state class sub-ranges (block above kClassRangeMain).** An 11-line comment was the only description of how
  the colour segment splits every device-generated range by state class, and how BuildDrawsCS routes a draw through the latch's class
  map and class table to a sub-range slot. The table layout lives only in that prose and in BuildDrawsCS.hlsl, and nothing ties the two
  except the comment. Suggestion: describe the class map and class table as structs shared with the HLSL (or generate one from the
  other), so the layout is code rather than prose. Failing that, document it in dclf-architecture.md. Original:
  "The colour segment's state class sub-ranges (SetClassPlan; DrawPipelines.h, kSetClassShift): every range a device-generated call
  draws is split by state class, one call per (range, class) with that class's signature, since an execution set's pipelines may differ
  in their shaders alone. Range kClassRangeMain is the colour draws', ClassRangeOfSlot(slot) a second-pass range slot's (the decal
  groups', then the terrain groups'), kClassRangeForward the forward staging half's (its sorted half follows each class's). BuildDrawsCS
  routes a draw by its set index through the latch's class map (BuildDrawsLatch::classMapOffset: [count, 0, 0, 0], then
  IndirectState::classOf) to its class and its index there, which the sequence stores, and takes its slot in the class's sub-range from
  the latch's class table (classTableOffset: [classes, kClassRanges, 0, 0], then kClassWords per class: the class's first sort key and
  size, then (first, capacity) per range). The counts of the main, terrain and forward sub-ranges are the classCounts words
  (ClassCountWord); a decal sub-range's is its group's (DecalCountWord). Must match BuildDrawsCS.hlsl."
- **Resources/GpuLayouts.h, BuildDrawsLatch and BuildDrawsConstants.** Nearly every field had a two- or three-line comment saying which
  passes use it and what 0 means elsewhere. The struct is one 400-byte latch shared by the depth phases, colour segment, shadow views,
  occlusion views and reflection faces, each using a different subset. It is hard to tell from the code which fields a given dispatch
  reads. Suggestion: split the latch into a common header plus per-family parts (or unions with named members per family), so the
  "0 elsewhere" notes disappear. The long static_assert list of offsets would then be per part.
- **Resources/GpuLayouts.h, the count words (kCount*Word).** About 30 constants each carried "Must match BuildDrawsCS.hlsl" with prose
  saying what each word counts and which pass writes it. The word map is scattered over a hundred lines. Suggestion: a single shared
  header (C++ and HLSL) or one enum, so there is one list and no "must match" notes.
- **Readback/Readbacks.h, VisibilityReadback, and Readback/Visibility.cpp, PrepareWrites and VisibilityDecode.** A 13-line doxygen
  block on VisibilityReadback, a 12-line one on PrepareWrites and a 7-line one on VisibilityDecode were the main explanation of the
  kAccumulated write path. Events go GPU to render thread to decode task to main thread, verdicts follow a "conservative rule" (visible
  in either of the last two decoded frames, no verdict yet, or an unanswered camera cut), stood-in and "wide" writes differ, and the
  decode visits only changed slots through five different change sources. There is also a full reference pass kept only for parity. The
  struct has about 60 fields owned by four different threads, marked only by comments. Suggestion: split VisibilityReadback by owner
  (render thread, decoder, main thread, counters) into separate structs so ownership is a type, not a comment. Consider whether the
  incremental decode is still worth it against the reference pass, or drop one of the two. The conservative rule and the cut handling
  should be in dclf-architecture.md ("kAccumulated written by DCLF" exists; check it covers the cut and wide writes). Original
  PrepareWrites rule text: "visible when in view in either of the last two decoded frames, when the listing has no verdict yet (visible
  until FadeStateCS's first), or while a camera cut has no decoded batch of its frame or later. Stood-in roots and their geometry, plus
  wide writes: every other listed fade root and non-layer geometry under one, and each plain entry root (visible when any of its
  geometries is). A slot changing class is rewritten."
- **Readback/Visibility.cpp, SetChain and CellRoots.** These relied on long comments for engine facts: receivers test kAccumulated at
  every level, a stale bit above an out-of-view geometry is harmless, NiNode's destructor clears children's parent pointers, and
  DrawWorld_BuildSceneLists (AE 0x14064bc20) sets children from the third on while children 0 and 1 are list entries. Suggestion: move
  these engine facts to skyrim-engine-notes.md, which the code already cited, and keep only the address in code.
- **Readback/RunRanges.h, RunRanges.** The 7-line block was the only explanation of why stable runs get a readback range and leased
  runs do not (lease indices change per run, so no later commit would read the data). This is a subtle contract with the runtime's
  lease model. Suggestion: document it in the ORG runtime docs next to org::RunRef::Stable.
- **Resources/FrameSlots.h, frame slot layout (kFrameSlotBytes and the lighting slot rows).** The layout of the frame constants buffer
  (vertex slots, pixel slots, shared light slot halves, lighting slot rows c0 to c20 and up) was spread over five multi-line comments.
  Suggestion: one table in dclf-architecture.md or a layout struct, since three files (FrameSlots.h, FrameRecords.cpp, Uploads.cpp)
  must agree on it.
- **Resources/Resources.cpp, AddShadowViewSlots and SetDrawArgsOwners (the slice-sharing comment).** Whether shadow views may share
  draw arguments depends on which views draw together in one frame (paraboloid halves do, a light's slice views do not, cascades and
  occlusion maps keep their own). That rule exists only in a comment. Suggestion: name it in code, for example a function that returns
  whether two placements can draw together.

### 2. Hazards kept

- GpuLayouts.h: kShadowDrawBytes. D3D12's SV_InstanceID ignores the first instance, so the D3D12 signature also writes it into the draw push word.
- GpuLayouts.h: offsets 256-aligned for D3D11 constant buffer offsets, at least 16 for the descriptor-buffer model's pointer loads.
- GpuLayouts.h: BuildDrawsConstants is 128 bytes, Vulkan's least maxPushConstantsSize, so no room for another word.
- GpuLayouts.h: stage word at byte 300 is read by byte offset in the shaders, so a second user must grow the latch.
- GpuLayouts.h: kCullNoNearPlane. Clamped views pancake casters onto the near plane, so the near plane rejects nothing.
- GpuLayouts.h: kCullMinTexels. DCLF is the only source of those sun casters, so a culled one casts no shadow at all.
- GpuLayouts.h: kCullFadeOnVisible engine address 0x142032dfd; kCullParabolic FUN_14151a1e0; depth target [2] FUN_1414b44f0.
- GpuLayouts.h: kClassUnplanned/Overflow counters must stay 0; a draw is never drawn with another class's state.
- Resources.cpp: one depth import that is both attachable and readable, else no barrier between depth draws and the HZB read.
- Resources.cpp: DXC compiles can block for seconds, so they run on the build executor, never the render thread.
- Resources.cpp: BasicRHI places whole buffers at 256 or more; pointer loads assume 16 (assert).
- Resources.cpp: draw-count view must cover every word, a short view reads zeros that look clean.
- Resources.cpp: BuildDraws appends in thread order, so without the sort nearly every sequence switches pipeline.
- Helpers.h: native mode puts async-compute passes on graphics, cross-queue waits cost about 0.13 ms against 0.03 ms of work.
- Helpers.h: GrowCapacity only grows (power of two) so the recording settles.
- FrameSlots.cpp: CS binds b5/b6 from Renderer_ResetState, so whether they are bound at capture depends on the last reset.
- FrameRecords.cpp: point light table must put shadow lights first; the shader counts the first ShadowCount as shadow lights.
- FrameRecords.cpp: frame lighting is latched every commit, since latching on change would change the frame's shape.
- IndexPool.h: ranges never shared, since freed buffer addresses are reused at once.
- Visibility.cpp: a kept verdict must still be listed, so a freed node's verdict cannot answer for another at its address.
- Visibility.cpp: write lists hold their publication's retirement node so their nodes stay alive.
- Visibility.cpp: DrawWorld_BuildSceneLists AE 0x14064bc20 behaviour (children 0 and 1 are list entries).
- Readbacks.h: Harvest runs after the host waited for the execution, and the next one is not submitted yet.

### 3. Possible bugs or dead code noticed

- Resources/GpuLayouts.h: a doxygen block for ShadowLatchLayout sat above ShadowSlotEntry, separated from its struct. I moved the
  shortened version to sit above ShadowLatchLayout (comment move only).
- Resources/GpuLayouts.h: kBucketTerrainPair, kForwardSortShader and the refraction range comment sat together with no clear owner;
  the refraction comment described no constant. Removed the dangling text into the forward/refraction lines; no code issue.
- Readback/Readbacks.h: kVisibilityBitInMargin (GpuLayouts.h) is reserved and unimplemented; possibly dead.
- Readback/Readbacks.h: writesExpired is documented as expected to stay 0 ("none: each holds its node"); if it is never non-zero the counter may be dead.

### 4. TODO/FIXME lines found

None.

### 5. Coverage

Done (all 14 files): Resources/FrameRecords.cpp, FrameRecords.h, FrameSlots.cpp, FrameSlots.h, GpuLayouts.h, Helpers.h, IndexPool.h,
Resources.cpp, SceneBuffers.h, SceneBufferSet.cpp, SceneBufferSet.h; Readback/Readbacks.h, RunRanges.h, Visibility.cpp.
Not done: none.
Comment lines (lines starting with //, /* or *): 922 before, 509 after. No comment block of three or more lines remains outside file
tops. Each file verified identical to HEAD with comments and whitespace removed.

## render-b part 2: Core/Render/Payload

### 1. Simplification or documentation targets

- **PayloadVersions.h/.cpp, the payload part version chains (PayloadPools).** The file header and the PayloadPools block explained seven parts, each a chain of GPU-built versions. Each version is built from its predecessor by a GPU copy plus journal runs plus "tails". Backing pools cap per class, and journals trim to the oldest backing's position, published by the producer after each build (PublishFloor). Readers have to hold four ideas together to follow a single post: journal floor, tails, held versions and shrink windows. Suggestion: a doc section in dclf-architecture.md with a diagram of one part's chain (source, held list, floor, delta and catch-up). Or simplify by dropping the "tails" special case: write the stream rows and frame inputs as their own untailed parts, so every part uses the same journal-only delta. Original header: "The scene payload (D5): seven parts, each a chain of GPU-built versions ... the part's journal's runs since its last version and its tails, uploaded once, the rest GPU-copied from that version on the streaming copy queue. The snapshot is posted once every version is GPU ready ... the frame that installs it catches nothing up".
- **Versioned.h, `sealed` vs `adopted` NamedVersions.** A long comment set the naming rule: `sealed` is the producer's set, `adopted` is the render thread's, a set's `serial` is its identity, and `contentEpoch` is a journal position. The comment was the only place these terms were defined, and they are used across the whole feature. Suggestion: give them distinct types (SealedVersions / AdoptedVersions) so the compiler enforces which side reads which, and define contentEpoch in the architecture doc.
- **ContentOwners.h, descriptor lifetime through byte ranges.** Descriptors stay alive because the bytes that name them are tracked by range, and each execution holds a snapshot of all owners. This is an unexpected way for lifetime to flow: a bindless row overwrite releases descriptors one snapshot later. The code alone does not show why. dclf-architecture.md "Descriptor ownership" exists. Make sure it covers the range-replace and snapshot rule, since the header now only summarises it.
- **MainBuild.h ResidentRegion, pair resolution bookkeeping.** About ten comment blocks explained witness hashes, owner refcounts, material and pipeline pair lists, check-list sources, the loop list, whole-scene mode and side bits. Together they are the only description of how the main build narrows which pairs it re-resolves. The struct holds roughly 25 cooperating fields. Suggestion: split pair tracking (pairs, witness, lists, owner counts) into its own class with a small interface (Acquire, Release, Resolve, ChangedPairs), and document the narrowing rule once there.
- **Uploads.cpp ResolveFrameTextures.** A four-line comment listed every frame texture source by feature (soft effects depth, TerrainBlending mask t55, CS feature views, capturedWhenNull). Further comments covered graph buffers, frame buffer copies, the character light engine target, precipitation occlusion and gone producers. One function special-cases five registration kinds, and the comments were the only map. Suggestion: one table-driven resolver per registration kind, or a doc listing which register comes from where.
- **Latched.h LatchRegionsHeld.** A six-line comment explained that a published copy's latch region is pinned to its lease, so snapshot-fixed bytes are written once per key, while leased runs get fresh regions. This is a caching rule whose correctness depends on lease semantics in ORG. Document it in ORG's latch block docs. The struct comment is now two lines.

### 2. Hazards kept

- Uploads.cpp ZeroCommitCounts: the colour segment must zero only its own draw word, or it erases the depth phases' counters before they are read.
- Uploads.cpp LatchedUploads::Finish (Uploads.h): unwritten copies are zeroed so a published copy's region never replays an older run's bytes.
- Uploads.cpp CountMainDraws: a draw count past the shape's sequences is a bug in the scene draw bound (hard fail).
- Uploads.cpp FillCullLatch: HZB mip 0 covers twice its size in source pixels.
- Uploads.cpp NoteExportRead: a D3D11 view of a published graph export is not ordered after the producer's graph writes.
- Uploads.cpp: the culling must use VS_PerFrame c8 ViewProj. Both epochs must rasterise identically for the colour EQUAL test.
- MainBuild.cpp LoopObject: outside the set, DCLF depth would break the engine's EQUAL test against its own prepass.
- MainBuild.cpp LoopObject: an undrawable decal must still reach BuildDraws as a zero-count draw, not a stale one (DecalSlot guard).
- MainBuild.cpp: without its vertex stream row, a face shape's other attributes draw as positions.
- MainBuild.cpp: a short Vulkan vertex or index slice reads zeros and the object collapses silently. Nothing on the D3D11 side sees it.
- MainBuild.cpp ResolvePair: colour alone would test EQUAL against depth nobody wrote.
- MainBuild.cpp: a character-light pass reads kCharacterLightRegister instead of t11. Effect key bit 15 is Membrane's (not ported).
- MainBuild.cpp AcquireBody: every idle body must be reset, or it pins its publication's retirement chain.
- MainBuild.cpp SetPairOwners: the counted owner keeps its object alive, so addresses are not reused.
- MainBuild.cpp: the input row word overflow is a hard failure.
- MaterialLookups.cpp: read before writing a lookup entry, because writing copies a shared chunk.
- MaterialLookups.cpp: the shadow set is built for one format. Other-format modes have no pipeline.
- MaterialLookups.cpp: the shadow inputs and resolution are replaced together so readiness and pipelines agree.
- PayloadVersions.cpp Parts(): never destroyed, because leases and build results arrive during teardown.
- PayloadVersions.cpp: backings per class = frames in flight + 2. One spare only, since extra spares hold VRAM (HexRim figures kept).
- PayloadVersions.cpp: a quiet part is rebuilt when its shrink is due, or it keeps its large class.
- KeptStores.h RowBuckets: depth-only pipelines first so alpha-tested casters fail depth sooner.
- MainPayload.h: material rows are journalled in four 256-byte parts because engine controllers animate one part every frame.
- MainRows.h AnimatedRows: an index moves only with its own row. Only MainBuild::Finish writes it.

### 3. Possible bugs or dead code noticed

- KeptStores.h ObjectRecordStore: `collisions` counts concurrent MakeStreamViews calls that "should never happen". This is a race detector left in production. Check whether it is still needed.
- PayloadVersions.h ScenePayload::mainBody and PayloadVersions::mainBody, and Uploads.cpp CommitMainPayload: "TEMP (set parity)" diagnostics are still present (see section 4).
- MainBuild.cpp: the static `slots` vector in AssignAnimatedRows relies on builds running one at a time. It is not thread-safe if that ever changes.
- Uploads.cpp WriteClassRegion: `return; // never` is an unreachable branch, per its trailing comment.

### 4. TODO/FIXME lines found

No TODO or FIXME. There are "TEMP (set parity)" markers, kept and shortened:
- PayloadVersions.h ScenePayload::mainBody, PayloadVersions::mainBody.
- PayloadVersions.cpp MirrorInputs (TEMP CS_DCLF_SET_PARITY).
- Uploads.cpp CommitMainPayload, two blocks (frame number stamping, body pairing check).

### 5. Coverage

All 18 files in Payload/ are done. Full-line comment lines went from 1010 to 550. Trailing end-of-line comments were mostly left as they were.
Each file is verified to be identical to HEAD once comments and whitespace are stripped, and every added diff line is a comment.
Note: Uploads.cpp has CRLF line endings in the working copy (HEAD is LF). The apply script keeps the existing ending, so it was already CRLF before the edit.

## render-b part 3: Core/Render/Shadow

The first agent on this part was cut off by the API limit before it wrote notes. The notes below were rebuilt from the diff of
what it removed, plus the 7 small files finished afterwards. Its own findings on possible bugs are lost.

### 1. Simplification or documentation targets

- **ShadowBuild.h `KeptShadowBuild` (persistent shadow inputs and material rows).** A 7-line comment was the only description of
  how per-mode caster regions change: by change-log entries, by rasterizer state changes, and when a waiting pipeline or diffuse
  becomes ready. The material row table is also explained only there, with slots kept per material, lowest free first and growth
  that delays a frame. A second block listed the five events that rewrite a row. Suggestion: give dclf-architecture.md
  "Persistent draw state" a shadow subsection, or move the material row table into its own small class whose methods name the
  events.
- **ShadowBuild.h `BuildShadowPayloadShared` / `BuildShadowPayloadRegion`.** A 7-line comment explained the two-half build. The
  shared half may run beside the main build because it never writes the main build's region. The region half runs later,
  maybe on another thread, and writes into the main build's region once that build is done. The thread and ordering contract is
  not visible in the signatures. Suggestion: make the handoff a type that only the shared half can create and only the region
  half can consume, or document the ordering in the architecture doc.
- **ShadowEngine.h (file top).** An 8-line header explained who owns which part of the shadow engine state: revision
  participant, ShadowHost, Impl, and the epoch frame half that reads it through Impl. The header said "each member says whose it
  is", so ownership is tracked by comments on every member. Suggestion: split the struct by owner (revision side and frame side),
  so the compiler enforces it.
- **ShadowCapture.cpp, rasterizer entry capture and the occlusion map view.** The comments were the only explanation of two engine
  interactions. CS's ShadowmapCascadeRasterizerFix swaps per-cascade bias copies into the engine table only during this window.
  The Utility shader sets the cull mode per pass, so DCLF reads the table at cull mode 1 rather than the renderer's current
  state. The occlusion map view is derived from the camera Skylighting just set up, and for the sky map it is a quarter of the
  square. Kept short in code. Suggestion: one doc section on the engine and CS state that DCLF captures at shadow time.
- **ShadowEpochs.cpp occlusion map readiness and `LatchOcclusionViews`.** Readiness depended on last frame's layout, the selected
  revision's shape, the installed publication's payload and a later re-check when the layout changed this frame. Latching then
  writes either real blocks or a zero latch per map view. Suggestion: a single "occlusion map state" record per map with an
  explicit state field instead of conditions spread over several functions.
- **ShadowCameras.h `ShadowCamera`.** The comments listed every consumer and where each camera board lives (Sun, Local, Focus
  views, portals, reflections, first person). The bit-exact port of the engine math (SetCameraData 0x140e56d70, SetFrustum
  FUN_140d32cc0, Process2 0x140e28690) is the real constraint and was kept. The consumer map belongs in the architecture doc.

### 2. Hazards kept

- ShadowCameras.cpp/.h: the engine math ports keep the engine's operation order so results match bit for bit (FUN_140d32cc0, FUN_140d32a60, FUN_140e57ae0, FUN_1404a6ed0).
- ShadowCameras.h: the culling planes are the frustum, replaced whole by customCullPlanes when doCustomCullPlanes is set (Process2).
- ShadowCapture.cpp: ShadowmapCascadeRasterizerFix swaps bias copies into the engine table only for the capture window, so read the entry then.
- ShadowCapture.cpp: the Utility shader sets the cull mode per pass, so the renderer's current cull mode is stale. Read the table at cull mode 1.
- ShadowBuild.cpp: texture transforms must be read before the walk, because float controllers move them between Main::Draw and BeforeShadowMaps.
- ShadowCapability.cpp: pipelines use the depth-stencil view's format, not the typeless texture's.
- ShadowCapability.cpp: occlusion targets must be imported in a fixed order up front. A later import rebuilds the graph mid-play.
- ShadowCapability.cpp: readiness reads the lookups' immutable inputs, never the render thread's growing readyModes.
- ShadowPoints.cpp: DrawsShadowViews is decided before the engine's first shadow map clear. The clears follow it.
- ShadowEpochs.cpp: the sun caster radius test is valid only for orthographic projections.
- ShadowShape.cpp: bucket capacities grow only to powers of two, so recordings change only on growth.

### 3. Possible bugs or dead code noticed

- ShadowCapability.cpp `ShadowEngine::PlacementOf`: falling back to any placement in the slot is described as a defect path. It is worth an assert or a log.
- The first agent's findings, if any, were lost when it was cut off.

### 4. TODO/FIXME lines found

None in Shadow/.

### 5. Coverage

All 17 files in Shadow/ are done. A comment-and-whitespace-stripped comparison with HEAD matches for every file.
Comment lines went from 910 to 555.

## render-b part 4: Revision/, Snapshot/, Views/

### 1. Simplification or documentation targets

- **Revision/Revision.h `RevisionRecordings` and Revision/Assemble.cpp `RequestRecordings`.** A host epoch (depth) records one
  recording per own variant times every subset of its hosted views being absent. The index layout lived only in a six-line comment
  ("for every subset of them absent, none absent first, its variants with the present ones' shapes..."). The producer and
  `IndexOf` must agree on a bit-trick layout, and the depth index-pool variant doubles it again (2 x 4 = 8 recordings). Suggest
  replacing the combinatorial recordings with one recording per variant whose hosted passes skip via a runtime flag or push word.
  Failing that, document the layout in dclf-architecture.md with a table.

- **Revision/Coverage.cpp `DecideEpochCoverage` / `DecideCoverage` / `DecideViewCoverage`.** Coverage has three paths: GPU,
  CPU pipeline, and forwardDirect exceptions. Each has its own rules for hosted epochs, gated classes, live sites and recordings.
  The rules were explained only by long comments. Suggest one coverage table per epoch kind, built once per frame, that names the
  inputs each kind needs, so the three paths do not each restate the rules.

- **Revision/Coverage.cpp `BuildPoint` and `BuildIfAsked`.** The build point is the only place graphs build. DCLF builds attach at
  frame end, others attach here, and a frame waiting for its snapshot must attach DCLF builds inline or the wait never ends. The
  comments carried all of this. Document the build point and attach timing in one place (dclf-architecture.md, a "graph builds"
  section). Consider making the "attach now while waiting" case explicit in the runtime API rather than a flag argument.

- **Revision/Producer.cpp `RequirementsMoved`, `TablesWitness`, `LookupsWitness`, `VolatileWitness`, `RevisionShapesKey`.**
  Revision reuse depends on hand-maintained hashes that must cover exactly what the requirement pass and the shapes read. The
  original doxygen listed every field and every reader. Any new read that is not added to a witness silently keeps a stale
  revision. Suggest deriving the key from the inputs the pass actually reads, for example by routing reads through a recorder,
  or at least keeping CS_DCLF_PERSISTENT_PARITY on in a test configuration so drift is caught.

- **Snapshot/Snapshot.cpp `SnapshotPass`, `WorkToBuild`, `PublishPayload`, `NoteScenePassDone` (publication gate).** The builder,
  the coordinator and the frame pace each other through the publication gate, `publicationGated`, `snapshotWorkTaken` and
  `frameWaiting`. The handshake, with its seq_cst set-flag-then-recheck pattern, was explained only in several long comments
  spread over four functions. Suggest a small gate object that owns the flags and the wake, with its protocol documented once.

- **Snapshot/Snapshot.cpp `AdoptCurrentSnapshot`.** The wait loop serves owner-thread build work, gives back tickets, flushes
  DXVK's stream, nudges ORG's host thread and advances retirement, all because nothing else runs while the frame waits. The
  original comment was a six-line list. These are hidden progress dependencies, and missing one hangs the game. Suggest making
  the waited-for work independent of the render thread, or else listing the dependencies in dclf-architecture.md.

- **Snapshot/ViewBatchesInternal.h `SceneLists` and Snapshot/SceneLists.cpp.** The incremental lists (base, tombstones, delta,
  volatile lanes, append-only store, async compaction) were described in dense header comments. dclf-architecture.md already has a
  section. Keep it current, because the code comments are now short.

- **Views/ViewEngine.cpp `ViewSlotReady` and `ViewDrawable`.** Readiness must resolve from the scene lane's own catalog and the
  published targets, never the render thread's copies, and must match what the revision shapes resolve. Two sources of truth for
  "can this slot draw" must agree. Suggest one shared resolver used by both the lane and the shapes.

### 2. Hazards kept

- A recording made on another graph build does not belong to the revision. Revision/Assemble.cpp
- Copy shapes, because live recordings are prepared against the live shape's identity. Revision/Assemble.cpp `Kept`
- Wait for an asked-for graph build before recording, or the recording uses the graph about to be replaced. Revision/Assemble.cpp
- A revision is made only while its build state is the attached one. Revision/Revision.h, Revision/Producer.cpp `MakeRevision`
- Use only the publication's lookups and catalog, never the frame's installed ones. Revision/Producer.cpp `MakeRevision`
- Shapes size from slot capacities, not tables, because a later publication may hold more draws. Revision/Producer.cpp `MakeMainShape`
- Plain buckets are all or nothing, or a frame draws some draws twice or not at all. Revision/Producer.cpp `LayOutSetClasses`
- Slot routing and terrain twin buckets must match BuildDrawsCS. Revision/Producer.cpp, Snapshot/ViewBatchesInternal.h
- Latched copy order must follow CommitMainPayload and CommitView. Revision/Producer.cpp
- Latch layout keys give a new layout a new block, so frames in flight keep the old one. Revision/Requirements.cpp
- Bucket map covers every pipeline slot, since a commit past the map draws nothing. Revision/Producer.cpp `RequireRevisionSet`
- Draws must embed versions from the sealed set, or a shader reads a released version and loses the device. Snapshot/Snapshot.cpp `BuildSnapshotDraws`
- Set-flag-then-recheck on the publication gate (seq_cst) to avoid a lost wake. Snapshot/Snapshot.cpp `WorkToBuild`, `PublishPayload`
- Re-wake after a failed pass, because the failed pass may have consumed the wake. Snapshot/Snapshot.cpp `SnapshotPass`
- The snapshot pump is never destroyed, because a completion may wake it during teardown. Snapshot/Snapshot.cpp `SnapshotPump`
- Flush DXVK's stream while waiting, or a run in this thread's stream never completes. Snapshot/Snapshot.cpp `AdoptCurrentSnapshot`
- BindAdopted must run after the frame's last submission. Snapshot/Snapshot.cpp
- Sequence backings are never patched in place. Snapshot/ViewBatches.cpp
- Lane index is 16 bits, so batch lanes cap at 65528. Snapshot/ViewBatches.cpp
- Occlusion map view bits must match GpuLayouts.h. Snapshot/ViewBatchesInternal.h
- Published textures are read by graph id, because an import of the export is not ordered after the producer. Views/ViewEngine.cpp
- The view's depth must match the main pass format for forward keys. Views/ViewEngine.cpp `ViewSlotReady`

### 3. Possible bugs or dead code noticed

- Revision/Assemble.cpp `RecordingCompleted`: when `dropped` is set from a build-generation mismatch with a non-null recording,
  `a_error` is null, so the fragment fails with "recorded on another graph build". Fine, but the recording is still counted in
  `remaining` and the `outstanding` decrement happens for each. Looks correct; noted only because `failed` is set once and later
  successes are ignored.
- Snapshot/Snapshot.cpp `PostSnapshot`: the removed comment called "frames with no epoch after a target format change" an open
  defect. The code makes such a snapshot stale until the new catalog exists. Worth confirming the defect is closed.
- Revision/Producer.cpp `RevisionShapesKey`: local lambdas `ptr` and `raw` in `MixMainShapesKey` and `ptr` in `RevisionShapesKey`
  may be unused in some paths. Compiler warnings would tell.

### 4. TODO/FIXME lines found

None.

### 5. Coverage

All 17 files done: Revision (8), Snapshot (5), Views (4). Comment lines, as counted by the checker (any line starting with a comment
or carrying a trailing comment): 1083 before, 562 after. Every file was checked mechanically: with comments and whitespace removed,
each is identical to HEAD.

