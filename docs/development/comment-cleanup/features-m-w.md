# Comment cleanup notes: features-m-w

Worktree: test-runner. Slice: src/Features/DrawcallLimitFix/Features/ sub-folders MainView to Water, plus FeatureList.cpp, FeatureList.h, Manifest.cpp.

## 1. Simplification or documentation targets

- Shadows/Local/LocalLightCull.cpp (CullEmpty, NoteMainRegistration, filter build/UpdateFilter, NodeOnVisible): point-light culls are replaced by three cooperating mechanisms (category filter on NiNode::OnVisible, "empty" culls with hand-written Accumulate bookkeeping, Subset culls for engine-only casters) plus sphere-bit replication at the first clearing main registration. The comments were the only place the modes and their interactions with portal-strict lights, late lights and parity frames were explained. Suggest: one doc section (dclf-architecture.md) describing the light-cull modes as a state table, and collapse Empty/Subset/Engine selection into one function returning the mode with the reasons enumerated.
- Shadows/Sun/SunAccumulation.h class SunAccumulation + SunAccumulation.cpp (FullFrustum, AppendNonAccum, CascadeProcess1, Register): three stacked levels (registration skip, entry exclusion via insertion veto, whole skip via empty scene lists) each with its own frame flags (bitsReady, exclusionLive, whole, skipExcluded). A 19-line class comment was the main explanation. Suggest: document the three levels and the bitsReady window in skyrim-engine-notes.md / dclf-architecture.md; consider merging the exclusion and whole-skip states into one per-frame enum.
- Sky/SkyCapture.cpp (SkyBatch, CloudGroup, Capture::deferred/deferralBroken): the sky batch capture may "span" the following cloud group call, finishing late; a missing cloud call (another hook) breaks spanning permanently. This cross-hook deferral is non-obvious. Suggest: hook the FinishAccumulating region once (sky batch through group 13) instead of two hooks sharing deferred state.
- Sky/SkyCapture.cpp SunOcclusionQuery: engine's BeginQuery/EndQuery are neutralised and an ORG query answers GetQueryData, skipped draws answer the last count. Document in an occlusion-query doc.
- Engine draw family headers (Water/Water.h, Sky/Sky.h, ShadowMask/ShadowMask.h, MenuViews/MenuScenes.h, Precipitation/Precipitation.h): each carried a 15-30 line top comment re-explaining the same latch-at-DrawTriShape mechanism and an identical Available/Reserve/Setup/CanTake/ReadPublished/Program/Keep/Submit/Status API with copy-pasted doxygen. Suggest: one Core/Engine/Family doc for the mechanism, and a shared family-client template so each family header only lists what differs (slots, constants layout, views).
- Manifest.cpp top comment (19 lines): the only description of the cross-feature register/flag/word/phase allocation scheme and which install checks back each list. Suggest: a short doc (dclf-extensions.md) with a table of each shared resource kind, its range, and its runtime check; the file comment then points there.
- Shadows/Local/LocalLightCull.h: two 13- and 22-line free-floating comments explained the category filter and the "point lights' bits"/empty culls; together with the .cpp this is the primary description of point-light cull replacement (see LocalLightCull.cpp target).
- Particles/ParticleSnapshots.h class ParticleSnapshots: 27-line class comment was the main explanation of the writer/reader split (update-slot thunks on arbitrary threads, triple buffer, immutable record table with writersInside reclamation). Suggest moving the threading and reclamation protocol to dclf-architecture.md 'Particles', or replacing the hand-rolled table/count with a shared RCU-style helper used elsewhere (PortalViews::VisibleSetHold is a second hand-rolled copy of the same idea).
- TreeWind/TreeAnimation.h Install: 25-line comment was the only explanation of two engine-list interventions (animation list removal with detoured add/remove/clear; per-loop write-and-restore of multibound cull results on the tree list). The write-and-restore of engine cache values around an engine loop is a surprising interaction. Suggest documenting in dclf-architecture.md 'Owned trees off the tree manager's animation list' (already referenced) including the tree-list half, or replacing the write-and-restore with a detour of the loop's visibility test.
- Portals/StrictGates.h (and LocalLights' LightContextResources, PortalPrograms.hlsli StrictGateRejects): portal-strict point light gating is implemented three times (CPU Admit, GPU verdict publication, parity model) to match the engine's BSParabolicCullingProcess mode 4 behaviour; the 12-line file comment was the primary explanation. Already referenced in dclf-portal-query-batches.md; keep that doc authoritative and consider dropping the CPU path once the GPU path covers receivers.
- Reflection/Reflection.h top comment (23 lines): the main description of the split between the Reflection feature and Core's view engine (ViewSlots, ViewEpoch, PassCapture residue watch, LOD roots owned by face). The split is hard to see from code because it runs through callbacks across Core/Views, IndirectDraws and Features/Lod. Suggest a "view engine" section in dclf-architecture.md listing what a view feature supplies and what Core does for it.
- OcclusionMaps/OcclusionMaps.h: who drives the two maps depends on whether Skylighting is loaded (Skylighting drives both, else the DCLF shell drives the mask through its own hook of the engine's occlusion point), and Skylighting calls back into the shell. A two-driver arrangement explained only in a 20-line comment. Suggest a single driver (the shell) that Skylighting registers with, and doc in dclf-architecture.md.
- TreeLod/TreeLod.h top comment (17 lines) narrated the file map and Core's FamilySlots responsibilities; the Core/feature split for draw families deserves a doc section rather than per-feature narration.
- Shadows/Local/Local.h top comment (24 lines) mapped the feature's three unrelated responsibilities (light/focus views, point-light culls, the sun+paraboloid exclusion build) and why the build sits here (dependency direction with Shadows/Sun). Suggest moving the exclusion build into its own feature or Core so the placement does not need explaining.
- Registration order coupling (Reflection/ReflectionFeature.cpp, OcclusionMaps/OcclusionMapsFeature.cpp): switches and toggles must be registered in a specific order so toggle bits in the packed word stay 'where Core's were'. Hidden ordering dependency; suggest explicit bit assignment or name-keyed persistence.
- Shadows/Sun/Sun.h + Shadows/Local/Local.h: the Sun registration thunk does main-registration work on behalf of Core (owned masks) and Shadows/Local (point-light bits via a static call), creating a hidden Sun -> Local dependency and making point-light culls silently depend on the Sun feature. Suggest a registration-observer list owned by Core that both features subscribe to.
- FeatureList.cpp kFeatures: a 21-line comment lists the ordering constraints the array order encodes (early hooks, move-writer slots 2-4, pool order, toggle dependencies, Fades last). Kept intact as a hazard list. Suggest replacing implicit list order with declared dependencies (each feature declares DEPENDS/after) and a checked topological order.

## 2. Hazards kept

- Shadows/Local/LocalLightCull.cpp: Main::Draw's mask clear FUN_1414cb640 runs only in third person.
- Shadows/Local/LocalLightCull.cpp: NiNode::OnVisible filter skips other hooks in that slot (Grass Optimizations), so it stays out when one is present.
- Shadows/Local/LocalLightCull.cpp: Empty cull avoids the accumulation context lock against the main camera's list jobs.
- Shadows/Local/LocalLightCull.cpp: static BSParabolicCullingProcess is shared; its sphere is a light's only during that light's cull.
- Shadows/Local/LocalLightCull.cpp: filter snapshots retired two Presents later since a detour may still read one.
- Sky/SkyCapture.cpp: no depth bias offset for sky draws, else the deferred pass lights/fogs the sky as geometry.
- Sky/SkyCapture.cpp: Terrain Blending D3D11 depth must be read by heap index or the sun's depth test reads zero.
- Sky/SkyCapture.cpp: SkyShaderHacks runs only at state flush, which latched draws never reach.
- Sky/SkyCapture.cpp: FinishAccumulating calls group 13 only when non-null with flag 1 (SE 99938 +0x260, AE 106583 +0x259).
- Shadows/Sun/SunAccumulation.cpp: SE full-frustum call site is a tail JMP, patched as one.
- Shadows/Sun/SunAccumulation.cpp: each full-frustum process must keep one entry, since the cascade cull sets planes from the first.
- Shadows/Sun/SunAccumulation.cpp: null job list makes empty culls run inline; only the render thread reads the job list.
- Shadows/Sun/SunAccumulation.cpp: detours of the scene-list function read the outer BSTArray's size; pass the full container.
- ShadowMask/ShadowMask.h SlotRead: latching the shadow map from t3 left the sun's masks fully shadowed; DXVK's stencil view keeps red at zero so t6 is never sampled.
- MenuViews/MenuScenes.h LightingSlotRead: engine leaves stale views (a depth plane after a world frame) in unset slots; undeclared slots must be latched null.
- MenuViews/MenuScenesCapture.cpp LatchPointLights: Light Limit Fix leaves the PerGeometry point light rows empty, so the scene build fills them from strict lights.
- Sky/SkyDraws.cpp StagesOf: sun glare vertex stage + vertex pull makes DXC 1.9 emit invalid SPIR-V (VUID-10684).
- MainView/MainViewPasses.cpp RecordColourPreprocess: NVIDIA idles the GPU per device-generated call in eight-target passes unless preprocessed; state list must match the execution exactly.
- Shadows/Engine/ShadowEpochPasses.cpp: D3D12 layout is root constants only; the shadow signature also reads the instance word.
- Portals/PortalViews.cpp: engine WithinFrustumDistFirst has a rectangle bug accepting planes wholly outside view; start-room planes judged by PlaneJudged instead.
- Portals/PortalViews.cpp: VisibleSetHold is lock-free because MSVC's atomic shared_ptr locks; readers count before loading (seq_cst).
- Portals/PortalViews.cpp: Encode must not throw (runs in Build's exit guard and in the light walk's swapped state).
- Shadows/Local/Diagnostics/StrictLightParity.cpp: Address Library 1.7.104 assigns AE sub-index id 76792 to the segmented shape's OnVisible.
- TreeWind/TreeAnimation.cpp: detached-node map is never destroyed since engine objects must not be released during process teardown.
- Particles/MasterParticles.cpp: lock order gate then manager lock; removal hook runs on a job thread with no lock held.
- Precipitation/Precipitation.h: water leaves its temporal mask bound and rain writes its normal there, which the water's temporal blend reads.
- Precipitation/PrecipitationDraws.cpp: Particle.hlsl compiled with HLSL 2018 language version for FXC-style vector ?: semantics.
- ShadowMask/ShadowMaskCapture.cpp TakeDraw: latched draws skip SetDirtyStates' hook, where Volumetric Shadows, Exponential Height Fog and Skylighting take the sun cascades from t4; the family hands them over itself.
- TreeWind/Diagnostics/TreeWindParity.cpp: never copy the current frame's wind buffer, since the compute queue writes it beside the epoch with no ordering.
- TextureTransforms/TextureTransformsPool.cpp: Main::Update flips the TexcoordOffset double buffer; controllers write the other one, so reads within the window see the frame's value.
- Portals/Diagnostics/PortalCapture.cpp: async compute can run frames behind the immediate context; checks must use the words of the frame the dispatch read.
- TreeWind/TreeWindObjects.cpp: engine shifts the fast-sqrt pattern as signed (sar); reproduced bit for bit, matters for ferns' uninitialised distance.
- TreeWind/TreeWindObjects.cpp DeriveTreeAnim: must skip the previousWindTimer write or trees advance twice a frame.
- FeatureList.cpp: feature list order is load-bearing (kept the full constraint list).
- Particles/MasterParticles.h: engine removes a master when refcount < 3, so DCLF must hold masters by key only.
- TreeWind/TreeWalk.h: UpdateSelectedDownwardPass shrinks a tree's multibound bound ~15% with no event; a move event is pushed after it.
- MainView/MainViewPoints.cpp: DCLF's objects must reach the G-buffer and depth before Terrain Blending's held terrain is drawn.
- Shadows/Sun/SunViews.cpp: the caster list holds a second directional light that is never drawn; take the sun alone.

## 3. Possible bugs or dead code noticed

No code bugs found. Comment-level problems fixed in passing:
- MenuViews/MenuScenesCapture.cpp TryLatchLighting: its doxygen block had been left orphaned above LatchPointLights (two stacked doc blocks); moved a one-line brief onto TryLatchLighting. Comment-only fix.
- Shadows/Local/LocalLightCull.h: an orphan doc block "Install: the portal-strict point lights take the empty culls too." sat stacked above SelectFrame's doc with no declaration; dropped. The "counters since the last Take" doc sat on kReachKindCount rather than the Stats struct.
- Shadows/Engine/ShadowCasterPoints.cpp: KeepLight's doxygen block sat orphaned above FocusAccumulation's (two stacked docs); moved a two-line brief onto KeepLight. Comment-only.

## 4. TODO/FIXME lines found

None in the slice.

## 5. Coverage

- All 167 files in the slice were reviewed. Every file with a multi-line comment block was edited. About a dozen small files were left unchanged because their comments were already short and useful (for example FeatureList.h, Portals/PortalBatchLayout.h, Portals/PortalBytecode.h, Portals/ParabolicCulling.h, Portals/PortalContextAtlas.h, Portals/Diagnostics/PortalViewsView.h, Shadows/Sun/Diagnostics/SunAccumulationView.h, Shadows/Sun/Diagnostics/SunCasters.cpp, MainView/MainViewFeature.cpp, Portals/StrictGates.cpp, Shadows/Local/FocusViews.cpp, Shadows/Local/LightViews.cpp).
- Comment lines (full-line plus trailing, rough count): 5079 before, 2933 after (-42%). Total file lines 30353 -> 28207.
- The pass concentrated on multi-line blocks, top-of-file comments and doxygen. Many one-line trailing field comments were kept as they are (most give a unit, offset or meaning). A second pass on one-liners in the largest files (LocalLightCull.cpp, PortalViews.cpp/.h, SkyCapture.cpp, SunAccumulation.*) could cut further.
- Verified mechanically: with comments and whitespace stripped, every touched file equals HEAD (script verify.py in the scratchpad folder). No builds, no git writes.
- Note for the coordinator: during the run the verifier once flagged src/Features/DrawcallLimitFix/Features/Decals/DecalOrder.h (the other slice) as changing code; it may have been mid-edit, worth re-checking.
