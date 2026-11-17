# Native mode: one frame graph, every resource tracked

Native mode (`CS_ORG_NATIVE=1`) moves to BasicRenderer's model:
- One graph a frame.
- Every resource except immutable textures and static vertex and index buffers is registered with the graph and declared by
  each pass that uses it.
- No epochs and no home states.

DXVK guest mode keeps its epochs; that machinery goes in Phase 8 of [org-full-port.md](./org-full-port.md).

**Why.** Native mode rests every engine image in GENERAL on Vulkan and COMMON on D3D12, and closes about 48 graph executions a
frame back to that state.
- On the RX 6600 that costs about 1.2 ms a frame in Vulkan. It was measured by forcing DXVK's images into GENERAL with
  `CS_ORG_RESTING_LAYOUTS=0`: shadow clears went from 0.14 to 0.38 ms, and depth-done and sky rose to native's times.
- On D3D12 the same pattern costs about 3.4 ms of barriers.

**Decisions.**
- One frame graph, not open per-hook executions.
- Exempt from tracking: every immutable texture (engine DDS files, streaming windows, GFx textures, icons, CS's file
  textures), and static vertex and index buffers.
- Tracked: the engine's dynamic ring and every CS buffer.

## Phases

| Phase | What                                                                                                          | State          |
|-------|---------------------------------------------------------------------------------------------------------------|----------------|
| 0     | Censuses: native immediate operations, writes after creation, anchor run shapes, undeclared accesses           | Done (wave 1)  |
| 1     | Every port declares what it reads and writes, in both modes                                                   | Wave 1 merged  |
| 2     | Native resources owned by the graph; versioned identities in place of rebuilds; exempt class                  |                |
| 3     | The engine's leftover GPU operations as static gated passes; rare ones through `ExternalClient`                |                |
| 4     | Six live region subgraphs plus DCLF's own epochs; home states off; async structural builds                    |                |

**Revised 2026-11-13.** Three rules shape the work:
1. No structural recompile on the hot path; a subgraph that must recompile does it asynchronously and is swapped in.
2. Large regions of work are subgraphs, so one recompiling or re-recording (DCLF) leaves the others alone.
3. Static passes first, then swapped backings (versioned resources), and no dynamic declarations.

The regions, in engine order:
1. shadows;
2. pre-main;
3. deferred;
4. engine draws and water;
5. post;
6. UI and present.

DCLF's revision-driven epochs stay separate.
| 5     | The same on D3D12                                                                                              |                |
| 6     | Cleanup                                                                                                        |                |

## Phase 0 switches

All of these are off by default. They report per window (world, menu, loading screen, map) every `CS_ORG_CENSUS_FRAMES`
frames (default 600). `tools/cs-test/configs/native-census.toml` turns them on.

- `CS_ORG_NATIVE_OP_CENSUS=1`: every native immediate operation, by kind, target and place in the frame.
- `CS_ORG_NATIVE_WRITE_CENSUS=1`: SRV-only textures and VB/IB-only buffers that get written after creation.
- `CS_ORG_EPOCH_STATS=1`: each anchor's runs a frame, and whether its runs differ in shape.
- `CS_ORG_VALIDATE_UNDECLARED=1`: raw bindless accesses to resources the pass did not declare.

## Wave 1 (2026-11-13, branch `org-native/integrate`)

Lanes, each on its own branch from `test/refs-sweep` 7805d529a:

- **`org-native/p0-census`**: the four censuses.
- **`org-native/p1-declared-reads`**:
  - Composite slots 6-8 and 10-15 declare their producers' published ids. SSGI publishes its four composite inputs.
  - DCLF frame textures, the engine-draw families and LLF's buffers declare the exact view their tables carry.
  - Until Phase 4 moves those tables into Prepare, a check logs `[ORG] declared view mismatch` once if a latched index is not
    the declared view's index.
- **`org-native/p1-cs-textures`**: these become persistent, published graph twins:
  - Skylighting's occlusion depth;
  - Terrain Blending's terrain depth, which DCLF draws into the twin;
  - Cloud Shadows' layers;
  - HDR's scene, output and UI textures;
  - Upscaling's hudless source.
- **`org-native/p1-outside-writes`**: writes outside passes become declared copy passes from staged upload buffers. Async
  epochs cannot record texture uploads.
  - the DynamicCubemaps zero-fill;
  - the 3D LUT;
  - the TerrainShadows heightmap;
  - ImGui's textures, now without hand-written barriers;
  - occlusion query readbacks.
- **`org-native/p1-fallbacks-dclf`**:
  - Natively, D3D11 fallbacks of published inputs are gone (IBL, SSS, EHF, sun rays, VL).
  - One `FrameAnchors::ImmutableTexture` seam for file textures.
  - DCLF's foliage owners and its CPU sites' frame constants are declared.
  - A survey of DCLF's shadow and view epochs is in [org-native-frame-graph-dclf-views.md](./org-native-frame-graph-dclf-views.md).

Fixed during integration:
- **VL read nothing natively.** It declared a Terrain or Cloud Shadows id that nothing published, so its generate never ran and
  the frame went black. The input is now empty when nothing publishes it.
- **Census build fixes.** A field name clashed in the census, and `RunShapes` needed qualifying.

Checks: guest and native quick runs match their baselines (native: 4% of pixels differ by more than 16, the same as two baseline
runs).

## Wave 2 lane: buffer view refs (`org-native/p1-buffer-refs`)

A buffer view ref is BasicRHI's raw SRV/UAV pair, made once per buffer, so no ORG view is needed: a pass declares the buffer
without views (`ShaderResource`/`UnorderedAccess` with an empty span), and the ref stays the buffer's for its life.

- **Helpers** (`RenderGraph/BufferRefs.h`):
  - `DeclareRef` / `DeclareLateRef` declare a buffer read or written through its ref.
  - `RefLatch` holds the ref a site latched at commit. `CheckDeclaredRefs` logs `[ORG] declared view mismatch` once in Prepare when
    it is not the ref of the backing bound to the preparation. A late-bound buffer's declaration resolves to its stand-in, so it is
    checked against its current published version.
  - `LateRefBuffer` makes a buffer the graph does not own declarable: a late-bound `VersionedBuffer` whose version the host
    publishes when the frame adopts it. Publishing costs no graph build.
- **Frame value pools:** the pixel rows and texture transforms passes wrote their pool through the latch's ref but declared it read.
  `FrameValues::DeclarePoolWrite` declares it written; both passes check `PoolRef`'s ref.
- **Installed payload parts:** each part is a `LateRefBuffer`, published by `MakeFramePayload`. The build, draw, shadow, view,
  texture transforms and CPU site passes declare them (`DeclarePayloadReads`; `DeclareFrameBuffers` includes it).
- **CPU view batch sequences:** a `LateRefBuffer`, published when a frame's slot takes the set. Every CPU site declares it and checks
  the set's ref.
- **CPU sites:** the shadow site declares the shadow constants, view blocks and index pool; the depth site declares its hosted
  views' blocks and frame constants.
- **Recording refs:** `RecordingBufferRef` resolves the pass's declaration instead of taking a raw handle.

Left for later phases:
- **The engine's dynamic ring** (`EngineBuffers.cpp`): its chunks are not graph resources and a frame reads several of them. Phase 2
  registers them; the refs stay raw until then.
- **Named versions** (`NamedRefs.h`): the readers declare the versioned buffers (vertex stream, tree LOD, faces). Grass's frame and
  visible tables were not checked: Grass is another lane's.
- **Phase 4** writes these refs into the tables in Prepare from the declarations (`DeclaredBufferRef`) and drops the latches.

## Wave 2 results (2026-11-13, with `org-native/p1-validator-gaps` merged)

The validator lane does three things:
- It notes the resources made immutable: SRV-only engine textures made with data, and static TriShape buffers.
- It counts reads whose published id the reader declared as "declared (interim index)".
- It declares the three raw texture reads: kMAIN Copy for water, kHDR_DOWNSAMPLE4 for the character light, kSHADOW_MASK for grass.
  It also drops LLF's D3D11 export imports.

Guest and native quick shots match their baselines.

**Undeclared accesses, world, native:** from about 1,100 a frame to 183 a frame, at 5 sites. Another 161 a frame are declared
reads with an interim index, and 1,263 a frame are skipped as exempt.

| Site                                   | A frame | What                                                                                    |
|----------------------------------------|---------|-----------------------------------------------------------------------------------------|
| LandscapeSeams.cpp:104                 | 100     | `LandscapeSeamsFix::QuadData`, D3D11 resources of CS's own                               |
| EngineDrawFamily.cpp:411 (frame slot)  | 32      | a 320x320 engine texture made without data; Cloud Shadows' layers through D3D11 views     |
| EngineBuffers.cpp:211                  | 30      | the engine's dynamic ring (Phase 2)                                                      |
| NamedRefs.h:15                         | 20      | DCLF's named buffers (face positions, grass shapes)                                      |
| FrameRecords.cpp:242                   | 1       | `Deferred::DirectionalShadowLights`, a D3D11 buffer                                      |

**With `org-native/p1-native-writers` merged:** Cloud Shadows' layer clears and copies run as graph compute passes at
SkyReflections, EarlyPrepass and ReflectionsPrepass. HDR's scene and UI clears and Skylighting's occlusion clear are passes too.
Native shots match the baseline.

Immediate operations, world: 250 a frame. 240 of them are DYNAMIC constant-buffer discard maps. A native Map is a CPU write into
upload memory with no GPU work, because no native GPU work reads those buffers; draws take their constants from their captures.
So these need nothing from the graph. The rest a frame:

| Operation                                              | Where                       | Owner                    |
|--------------------------------------------------------|-----------------------------|--------------------------|
| ClearRenderTargetView on the six `Deferred::*` targets | after Prepass               | CS (Deferred), Phase 1   |
| ClearRenderTargetView on `kTEMPORAL_AA_MASK`           | after ImageSpace run 5      | engine, Phase 3          |
| ClearRenderTargetView on `kWATER_1`/`kWATER_2`         | after EngineDraws run 3     | engine, Phase 3          |
| CopyResource `kMAIN` into `kMAIN Copy`                 | after ImageSpace run 4      | engine, Phase 3          |

**Open: `[ORG] declared view mismatch`.** One line each for `cs.dclf.z.pixel-rows` (frame value pool 5) and
`cs.dclf.z.texture-transforms` (pool 2), in guest and in the census run. Each is logged once, so how often it happens is not
known yet. It may be a preparation made ahead of the frame seeing the next frame's backing; that has to be checked.

## Wave 3 lane: Phase 1 residue (`org-native/p1-residue`)

The worklist of "Wave 2 results", each now declared or exempt:
- **`LandscapeSeamsFix::QuadData`** (LandscapeSeams.cpp:104, about 100 a frame): an IMMUTABLE structured buffer made with its contents,
  never written. The validator now exempts every IMMUTABLE D3D11 resource, whatever its binding: D3D11 refuses writes to it. The
  site's tag reads "family null view 48+" because `FrameSlot + 104` runs past the frame slots' range; it is the seam fix's t104+.
- **Cloud Shadows' layers at t26** (EngineDrawFamily.cpp:411): the main sky client lists every layer twin's published id
  (`ORGCloudShadows::PublishedLayerId`), and the capture reads the layer by id. A D3D11 layer (no twin) is still imported.
- **"Engine::Texture2D 320x320"**: the water's flow map (the size setting, 5, times 64), which the ImageSpace port's flow copies
  write through their import `cs.d3d11.engine.flow-map`. ImageSpace publishes that import at Setup (`PublishedResources::PublishImport`,
  `ImageSpace::kFlowMapId`), and the water's clients list the id, so t8 reads it by id.
- **Named versions** (NamedRefs.h:15): every reader declares them, grass's frame and visible tables included (its cull, arguments
  and main passes). `DCLF::RefOf` takes the ref through `RenderGraph::DeclaredRefOf`, which the validator does not note.
- **`Deferred::DirectionalShadowLights`** (FrameRecords.cpp:242): t98 is a frame buffer, which DCLF copies into a declared graph
  buffer (`cs.dclf.frame-buffer.t98`). The colour commit no longer imports the D3D11 buffer there, and names the copy even when the
  mirror has no contents yet (it reads zero for that frame).
- **The six GBuffer clears after the Prepass anchor**: `ORGDeferredClears` clears them in one render pass (clear load ops) last at
  the Prepass anchor, with the renderer's clear colour, and `StartDeferred` binds them with `SRTM_NO_CLEAR` while that pass is in
  the built graph. On whenever the render graph runs; `CS_ORG_DEFERRED_CLEARS=0` keeps the engine's clears.
- **`[ORG] declared view mismatch`** is frame-keyed: a latch keeps the frame it was set in (`RenderGraph::RefFrame`, advanced at
  `SealPresented`) and its last four refs. A late-bound entry is compared only while the latch and its current version are the same
  frame's (else counted as skipped); a versioned entry matches any recent ref. `[ORG] declared view mismatch stats` lines count checks,
  mismatches and skips per site every 10 seconds while they grow. The texture check (`CheckDeclaredReads`) compares an index taken
  at the build with the same build's declaration, so it has no race and is unchanged.

## Census results (autoload save, native Vulkan, RX 6600)

**Immediate operations, world, about 283 a frame:**

| Kind                         | A frame | Notes                                                                                                     |
|------------------------------|---------|-----------------------------------------------------------------------------------------------------------|
| Map write (discard)          | 240     | Almost all are dynamic constant buffers. Up to 42 renames of one buffer a frame, spread over the frame.    |
| ClearRenderTargetView        | 31      | Mostly Cloud Shadows' layers, cleared at the cube clears.                                                  |
| CopySubresourceRegion        | 4.4     | Cloud Shadows' layers.                                                                                     |
| CopyResource                 | 1       |                                                                                                           |
| ClearDepthStencilView        | 1       |                                                                                                           |

**Written after creation.** Three resources in a whole session:
- two DYNAMIC `Texture1D`s, tracked anyway;
- one SRV-only texture made with data, which the loading screen updates 143 times.

So "made with data and SRV-only means immutable" holds, with promotion for that one case.

**Run shapes, world:**

| Anchor            | Runs a frame | Shapes within a frame |
|-------------------|--------------|-----------------------|
| ImageSpace        | 12           | vary: 18 distinct     |
| EngineFrameClear  | 6            | vary: 7 distinct      |
| EngineShadowClear | 4            | vary                  |
| EngineDraws       | 3            | vary                  |
| EngineCubeClear   | 2            | vary: 6 distinct      |
| SkyReflections    | 2            | vary                  |
| EngineQueries     | 2            | same                  |
| EngineDepthCopy   | 2            | same                  |

Every other anchor runs once a frame.

**Undeclared accesses, world:** about 1,100 a frame natively and 57 in guest.
- **The validator over-reports natively.** It counts engine DDS textures, static TriShape vertex and index buffers, and
  streaming shells. These are graph-managed or mutable there, but belong to the exempt class.
- **Interim declarations.** `ShaderResourceIndex` at EngineDrawFamily.cpp:525, FrameBindings.cpp:130/200 and
  ORGLightCulling.cpp:544-545: these reads are declared now, but the index still comes from the published table.
- **Buffer view refs read without a declaration** (`BufferRefOf`): FrameValues.cpp:3129, NamedRefs.h:15, PayloadVersions.cpp:587,
  ViewBatches*, Common.cpp:26 and EngineBuffers.cpp:205.
  - ORG already declares buffer access: `ShaderResource`/`UnorderedAccess` on a buffer, with or without a view. A buffer's ref
    lasts as long as the buffer, so a ref worked out at commit stays right.
  - The work is declaring these buffers on their passes, and registering the ones the graph does not know (the engine's dynamic
    ring).
  - For versioned buffers, the ref written at commit must be the bound version's; Prepare checks that.
- **Raw texture indices that remain:**
  - EngineDrawFamily.cpp:409, `kMAIN` copy in a frame slot;
  - FrameRecords.cpp:242, `kHDR_DOWNSAMPLE4` and LLF's D3D11 exports;
  - GrassTables.cpp:168, `kSHADOW_MASK`.

## Phase 2 item 2 lane: versioned identities (`org-native/p2-versioned-ids`)

**What versions now:**
- **Versioned twins.** A `GraphResources` texture made with `Options::versioned` keeps one `org::VersionedTexture` slot (`Versioned()`).
  - Its passes declare the slot (`AnchorPasses::Bind::Srv/Uav` of the twin), and `Register` makes the slot the name's resolver.
  - A `Resize` with the same mip count publishes the new texture as the next version and calls `NoteNewVersions`: no build.
  - The D3D11 export is made again, so D3D11 readers fetch `SRV()` again.
  - ORG's `VersionedTexture::CreateImage`/`PublishImage` take exported images (`ExternalTextureResource`).
- **Published ids.** A versioned twin is published as its slot, registered as the id's resolver. Its export and `ShaderResourceIndex`
  follow the current version.
- **Converted:** Terrain Blending's blended depths and mask. Readers of their ids (SSS, EHF, sun rays, DCLF) no longer rebuild on a
  resolution change. The blend's own anchor is still built again, because its group counts and engine depths change.

**Structural rebuilds** go through `GraphResources::RequestStructuralRebuild` and log `[ORG] structural rebuild <n>`. That covers:
- a replaced or released import;
- a resize of a twin that is not versioned, or one with another mip count (the resize generation);
- a published id replaced by a new resource.

**Still rebuilding, and why:**
- **Twins not converted.** Every other twin's passes declare `Graph()` directly, through about 210 sites. A version's backing must
  not also be declared directly, so each owner has to move every declaration to the slot before setting `versioned`. Most owners
  also make a whole new state and register their client again on a resolution change.
- **Imports and engine targets** (`FrameAnchors::Import`, `EngineTargets`). About 117 import call sites hold and declare the
  `ExternalTextureResource`. HDR's target upgrades change the format, which is a new shape class.
- **Client registration, refresh, rearm and join** (`FrameAnchors.cpp` `Rebuild`). These are changes of passes, not of identities:
  readback commits in SkyDraws and DecalOcclusion, Screenshot, Terrain Shadows' heightmap, and IBL sources.
- **New ids** (`RequestRebuildFor` for an import or publication added) and builds that skipped an id.
- **Shader reload, DCLF graph conditions, profiling and test harness:** program or graph-shape changes.

**Known effect:** DCLF's `DeclarePublishedRead` latches the index at the build. After a versioned resize it may log
`[ORG] declared view mismatch` once per id until Phase 4 writes the index from the declaration.

## Wave 3 results (2026-11-13)

Merged: `org-native/p1-residue`, `org-native/p1-imgui-static`, `org-native/p2-versioned-ids` and `org-native/org-async-builds`. The
merged ORG commit is 5c53686, which holds the versioned image textures and `BuildEpochGraph`/`AttachEpochGraph`.

Guest and native quick shots match their baselines: 1.6% and 0.9% of pixels differ by more than 16. No graph or epoch failed.

**Phase 1 is complete.** In the native census, the world window has one undeclared site left: the engine's dynamic ring at
`EngineBuffers.cpp:211`, about 30 accesses a frame. Phase 2 handles it.

**Declared-ref mismatch check.** The check is now tied to the frame. It found 0 mismatches in about 600 checks, so the earlier lines
were the race between frames in flight.

**Op census, world.**
- Left: the three engine operations Phase 3 covers (`kTEMPORAL_AA_MASK`, `kWATER_1`/`kWATER_2`, `kMAIN` into `kMAIN Copy`).
- Gone: the six `Deferred::*` clears, now a clear pass at Prepass (`CS_ORG_DEFERRED_CLEARS`).

**ImGui.**
- Fonts are 4 versioned atlas pages and icons are the exempt class. No dynamic declarations are left.
- With `CS_ORG_IMGUI=1` the CS menu draws with fonts and icons in guest and native.

**Versioned identities.**
- Terrain Blending's outputs resize as versions.
- Every other twin, import and engine target still rebuilds, because its passes declare the backing directly. That is about 210
  `Graph()` sites and 117 import sites.
- Those rebuilds are logged as `[ORG] structural rebuild N (...)`. A native load logs 6: five engine targets replaced, and one
  resize.

**Not run:** ORG's own tests. CS builds ORG with its tests off; SARP's build turns them on for BasicRenderer's copy.

## Phase 4 item 4 lane: the build manager (`org-native/p4-build-manager`)

**What it does** (`src/RenderGraph/BuildManager.{h,cpp}`, owned by `RenderGraphRuntime`):
- The host runs with explicit builds (`SetExplicitBuilds(true)`) and `Desc::reportStaleAliasPlacements`. A rebuild request only marks
  the epoch.
- **Requests** go through `RenderGraph::Builds::Request/RequestAll/RequestFor` (any thread; off the owner thread they are applied
  at the next poll). Requests ORG makes itself (an extension added or removed, the contributors' `Poll`, `SetExternalEpochs`) are
  picked up the same way, from `GraphHost::RebuildRequested`.
- **The poll** (`RenderGraphRuntime::BuildIfRequested`) runs at a frame boundary: DCLF's build point at the frame's start, or
  `SealPresented` in a frame without one (menus, loading screens, DCLF off). It attaches finished builds
  (`AttachEpochGraph`), asks again for epochs whose alias placements went stale, and hands every other epoch asked for to the
  manager's own thread (`BuildEpochGraph`). It never uses the runtime task pool: a build waits for pipelines compiled there.
- **Policy.** One build per epoch in flight. The epochs handed out at one poll attach together, once all are built. A request made
  after a build was handed out drops it and builds again (three times in a row at most, then it is attached and built again after).
  A rejected attach asks again; four in a row count as a failure. A failure keeps the old graph, is logged once, and is not retried
  until a new request (`GraphHost::RebuildRequests`).
- **Alias placement** of an off-thread build waits for its first preparation after the attach (ORG
  `PlaceDeferredPersistentAliases`, on the host's thread). Placing during the build moved the backings of transients the
  epoch's running graph shares, and that graph's next preparation failed with "Binding replacement requires a structural alias
  edit" (native, every load). The host's thread now also reads its mailbox between first tickets and between revision copies, so
  an attach waits for one step of a load's backlog, not all of it.
- **Deferred effects** (`Builds::Defer`). An extension's build-time effect waits for the attach: the frame anchors record the snapshot
  a graph was built from only then, and a failed build drops that snapshot's clients.

**Still on the render thread, and why:**
- **DCLF's epochs** are owner-built at its build point (and mid-frame when its main resources are made again). Its extensions read
  state the render thread changes in place: view and shadow resources (`r.target`, `shadow->depth[i]`, `AddShadowViewSlots`) and
  the graph conditions, and `GraphBuiltForCpu` flips at the request. Moving them off-thread needs copy-on-write DCLF state.
- **Shader reload** builds every graph on the owner thread with the host's thread held (`RequestAllSynchronous`): the pipeline
  registry's publish needs that safe point. Managed builds never publish.
- **Startup builds:** an epoch with no graph is built at its first submission, and `FrameAnchors::BuildPending` builds at setup.
  After the first world frame `BuildPending` hands its builds to the manager.

**Logs.** `[ORG] build manager: <epoch> attached: built in X ms off the render thread, attached in Y ms ...; why: ...` per attach.
`[ORG] build manager: graph built on the render thread after the first world frame (...)` for each synchronous build, naming the
caller. A statistics line every 3000 frames. DCLF's snapshots are keyed on its own epochs' build generations, so an anchor attach
no longer makes DCLF wait for new recordings.

## Build manager (2026-11-13, `org-native/p4-build-manager`, ORG 5db9d6c)

Every epoch rebuild goes through `RenderGraph::Builds`. A dedicated thread builds the graph with `BuildEpochGraph`, and it is
attached at the frame boundary with `AttachEpochGraph`.

**Results.**
- Toggling a feature (SSGI) or HDR rebuilds the anchors off the render thread in about 1 ms. In the world, an attach costs
  0.1-0.4 ms (median 0.25 ms native, 0.08 ms guest).
- Guest and native quick shots match their baselines.

**Fixed on integration: a native load failure.** A load failed with `Binding replacement requires a structural alias edit`. A
detached build placed its transients in the shared alias pool, which moved the backings of resources the running graph of the
same epoch still used. Alias placement now waits for the new graph's first prepare after the attach, which runs on ORG's host
thread.

**Left.**
- **DCLF's epochs still build on the render thread**, 3-8 ms each, 13 warnings in a native load. Their extensions read state the
  render thread changes in place; the fix is copy-on-write DCLF state.
- **Shader reload is still synchronous.**
- **Attaches during loading reach 14-19 ms.** The render thread waits for the host thread to stop for the swap. The host thread
  now stops between tickets and recordings; the rest is the statistics heap sizing and one preparation. In-world attaches are
  under 1 ms.
- **A new graph's first prepare now includes alias placement and one validated build**, on the host thread. A ticket asked for
  right after an attach may wait for it; watch `WaitTicket`.

## Phase 2 item 2 lane: versioned twins, part B (`org-native/p2-twins-b`)

**Helpers** (additive):
- `AnchorPasses::ClearOf(name, twin)` and `CopyOf(name, source, target, w, h)`. `Clear` and `Copy` take a slot (`slot`, `sourceSlot`,
  `targetSlot`) in place of the resource.
- `GraphPasses::MakeUavClear(twin, value)`, and `MakeUavClear`/`MakeUavClearUint` of a slot.
- No slot form of `AnchorPasses::Draw::target` yet. A raster pass into a versioned twin declares the slot itself.

**Converted:**
- **Screen Space Shadows.** Its output is versioned. The clear and the raymarch declare the slot. When `kSHADOW_MASK` changes size,
  `Prepass` resizes the output as a new version. Nothing is built, because the group counts already come from each frame's
  dispatch list.
- **SSGI.** The history textures and the exported outputs are versioned. Every step, clear and copy declares them through the twin.
  - When `kMAIN` changes size, `Prepass` resizes every texture. The versioned ones become new versions, so the composite's reads
    of the published ids are not built again.
  - The aliased transients are new resources, so the chain's own anchor is built again, through the build manager. The chain
    registers again, because its accumulation copy bakes its size. The history is cleared again.
  - Before this, SSGI kept its setup size after a resolution change.
  - A settings change keeps the textures of the old state that have the same name and shape. The published outputs stay the same
    resources, so only DeferredPasses is built again. A change between full and half resolution still moves an output id to
    another texture, which rebuilds every graph once.

**Skipped, and why:**
- **Never remade:** these keep one size for the session and are made once at setup:
  - Volumetric Shadows' copy, IBL's SH textures, Skylighting's volumes and occlusion map;
  - Cloud Shadows' occlusion, layers and scratch;
  - Grass Collision's buffers and texture, Terrain Shadows' textures, the glints noise, and Bink's movie texture.

  Their rebuilds are pass changes, not identity changes: import refreshes, Terrain Shadows' heightmap upload, IBL's sources, and the
  glints shader reload.
- **Aliased transients:** Subsurface Scattering's two textures and Volumetric Lighting's volumes. They are placed at each build, so
  they cannot be versioned.
  - Volumetric Lighting makes a set of passes for each volume size, so a new size is structural. It registers again through the
    build manager.
  - Subsurface Scattering's textures keep their setup size after a resolution change, as before.
- **Bink's targets** are imports: the framebuffer through `EngineTarget` (the `p2-imports` lane), and HDR's UI twin, which stays
  unversioned (`p2-twins-a`).
- **Not twins:** the `Graph()` hits in DCLF's `GpuTextures.cpp` (the host's graph), `ScheduleWindow`, `GpuLoadPass` and
  `PerformanceOverlay` (its frame-time history). `ImGuiBasicRHI`'s atlas pages are already versioned, and its other textures are
  the exempt class.
- **DCLF's Terrain Blending integration** reads `ORGTerrainBlending::TerrainDepthGraph()`. That terrain depth twin is Terrain
  Blending's own and is not versioned.

**Logs.** `[ORG] Screen Space Shadows: the shadow texture is now WxH`. `[ORG] Screen Space GI: its textures are now WxH`.
`[ORG] SSGI::AO: resized to WxH as a new version` for each versioned texture.

## Phase 2 item 2 lane: versioned twins, part A (`org-native/p2-twins-a`)

Owners: Dynamic Cubemaps, Exponential Height Fog, Effects11 (sun rays and the FX runtime), Light Limit Fix (culling, local
shadows), Post Processing, Upscaling and HDR Display.

**Converted: Exponential Height Fog.**
- Its volumes, conservative depths, histories and integrated volumes are versioned twins. They are kept across builds, and
  every pass declares their slots.
- A new grid extent (a resolution change, the dynamic resolution, a grid setting) resizes them in place as new versions. No
  client is registered again and nothing is built. Log: `[ORG] Exponential Height Fog: the volumes resized as new versions`.
- The commit's group counts follow the extent now. The history copies record no extent: zero extents copy the whole version.
- The integrated volumes are published once, so DCLF's reads of their ids never see a replaced resource.
- A change of inputs, history or group reduction still registers new passes, over the same textures (the Prepass anchor
  only, through the build manager). On a real resolution change the scene depth's import changes too, so the Prepass anchor
  is built again until the imports lane versions engine depths. Every other epoch is left alone.
- **Cost:** the volumes written and read within a frame (VBufferA, LightScattering, ConservativeDepth) are no longer
  aliased. A version's backing is made with it, not placed at a build.

**Not converted, and why:**
- **Dynamic Cubemaps:** every twin has the engine cubemap's fixed size and is never remade. Its rebuilds come from imports and
  shader reloads.
- **Effects11 sun rays:** its textures are transients made inside one graph, and new ones come with a new client, which never
  bumps the resize generation. The raster passes bake the dynamic-resolution viewports, and the shape holds the kMAIN and
  depth imports, so a size change stays a client rebuild of PostComposite.
- **Effects11 FX runtime:** the plan is per shape. It bakes viewports into its draws and holds kMAIN, depth and output
  imports, so it stays a client rebuild of Tonemap.
- **Light Limit Fix culling:** buffers only. Buffers wait for `VersionedBuffer` (Phase 2 item 4).
- **Light Limit Fix local shadows:** remade only on a settings change, which usually changes the slot count. That is a new
  shape class, and a new buffer and latch size.
- **Post Processing:** the chain bakes viewports (FullViewport, DynamicViewport, mips) into its draws and holds kMAIN and
  kMAIN_COPY imports. Its images are transients per build. Its effect-owned twins (DoF focus, LUTs, local exposure, motion
  blur, glare, lens flare) are made new, not resized, and registered at each chain build, so they never bump the resize
  generation. Versioning them would save nothing while the chain rebuilds. The chain stays a client rebuild of Tonemap.
- **Upscaling:** Upscaling.cpp destroys and makes the upscaled and hudless textures again. The hudless copy draws into its
  twin, and `AnchorPasses::Draw::target` has no slot form. Both draws bake viewports, and the client holds kMAIN's import.
- **HDR Display:** HDRDisplay.cpp destroys and makes its textures again. Bink, Scaleform and Upscaling declare the backings
  (`GraphOf`), and the client bakes group counts and imports. A format change (an HDR toggle) is a new shape class anyway.

**Follow-ups that would let more of these version:** a slot form of `AnchorPasses::Draw::target`; viewports and group counts
from the latch for fullscreen draws; versioned engine targets (the imports lane). `p2-twins-b` added slot forms for
AnchorPasses Copy and Clear (`CopyOf`, `ClearOf`) and `GraphPasses::MakeUavClear(twin)`.

## Phase 2 item 4 lane: native buffers (`org-native/p2-buffers`)

**The dynamic ring** (`Engine/Buffers/DynamicRing.cpp`, `CS_ORG_ENGINE_BUFFERS=1`, on in native mode):
- The ring is a late-bound `org::VersionedBuffer` ("Engine::DynamicRing", `detail::RingVersions`). Each chunk is published as its
  version (`PublishLate`) when it becomes the current one. The readers' declarations resolve to a fixed stand-in, so a new chunk costs
  no graph edit.
- Readers declare it with `Engine::Buffers::DeclareRingRead`. The only reader is the menu scenes' pass: a particles draw pulls its
  positions from stream 1 (`LatchDynamicGeometry`, `EffectSceneConstants::streamRef`). No other family takes dynamic draws, and DCLF's
  scene geometry is TriShape buffers, never ring chunks.
- There is no ref latch: a frame reads several chunks, so a pass's refs are not one version's.
- The CPU writes the chunks in upload memory, so a read needs no barrier, and no upload goes into them, so the upload planner waits for
  nothing.
- **Lifetime.** In native mode a full chunk is parked until the frame it filled in ends (`RefFrame` moves at `SealPresented`). It then
  goes to `ReleaseAfterD3D11`, which in native mode waits for the graph's submissions made until then to retire. Before, a chunk was
  released with the submission count at the moment it filled. A draw latched from it earlier in the frame runs in an execution
  submitted later, so the chunk could be reused under it. Guest mode is unchanged: it still uses the D3D11 query.

**What the validator saw at `EngineBuffers.cpp:211`.** `Resolve` made every ref by handle (`BufferRefOf(device, handle)`), which the
validator always reports, so it counted static TriShape buffers as well as ring chunks. Now:
- a ring chunk's ref is `DeclaredRefOf` (its readers declare the ring);
- a static buffer's ref is `BufferRefOf(org::Buffer)`, which the validator judges by the ORG buffer: noted immutable at creation, so
  exempt;
- DCLF's prefetch asks `Buffers::Owns`, which makes no ref.

**Static vertex and index buffers** stay exempt. Their leases need no wait: there is no GPU copy. `MakeStatic` (engine buffers) and
`NativeDevice::MakeBuffer` write the contents into host-visible memory before the D3D11 handle exists, so no lease can be taken
before they are written, and the next queue submission makes the writes visible.

**`Map` in native mode** (`NativeDevice` `ContextMap`):
- A buffer the graph imported (`Native::DescribeResource`, which DCLF's leases, imports and copies go through) is marked
  `graphShared`.
- A Map other than NO_OVERWRITE, or `UpdateSubresource`'s CPU path, on such a buffer waits for the graph's work up to the last frame
  boundary (`GraphHost::SealedPoint`). `DO_NOT_WAIT` returns `DXGI_ERROR_WAS_STILL_DRAWING`.
- The current frame's executions are not waited for: a mid-frame wait names only sealed work. A buffer mapped once a frame before its
  readers run is covered.
- The DYNAMIC constant buffers (240 maps a frame) are never imported, so they stay free.
- Textures need nothing: Map writes a DYNAMIC texture's upload side or a staging texture, which only the context's own copies read,
  and those are waited for by their value as before.
- Each wait is logged at first, then at powers of two: `[ORG native] CPU access waited X ms for graph readers of a buffer (...)`.

**For the main session.**
- Native census (`native-census`): the undeclared validator should list no site in the world window. `EngineBuffers.cpp` should not
  appear; static buffers count as exempt.
- Look for `[ORG native] CPU access waited` lines. A few in a load are expected. Many a frame in the world would cost frame time;
  the line names the buffer's bind flags, usage and size.
- The ring's chunk count (`[ORG] engine buffers: the dynamic ring has N chunks`) may rise a little in native mode, since full chunks
  wait for the frame's end.
- Guest quick shots: unchanged. The menu scenes' pass also declares the ring in guest mode (a stand-in read).

## Phase 2 items 1 and 3 lane: graph-owned and exempt textures (`org-native/p2-native-owned`)

Native mode only, behind `CS_ORG_NATIVE_OWNED` (on by default; `=0` keeps the old behaviour). `NativeDevice::MakeTexture` sorts
every texture it makes:
- **Exempt:** only a shader resource, made with its contents, not DYNAMIC and without CPU access.
  - Its contents go in through a transfer on the graphics queue (`InitializeShaderRead`) that ends in the shader-read state, where
    it rests. Nothing waits at creation; the transfer's immediate-context value is its readiness.
  - Its description (`DescribeResource`) names the shader-read layout, so imports and DCLF's raw views read it there.
  - The immediate context moves it to Common and back around its own operations (`Immediate::LeaveShaderRead`).
  - `FrameAnchors::ImmutableTexture` returns it as one graph resource (`Native::ImmutableTextureOf`): an
    `ExternalTextureResource` marked `ExternalImmutableShaderResource`, made on the first call after waiting for the transfer, the
    same pointer on every call. It is not registered under the name.
  - A write by the context (an update, a copy into it, mip generation) promotes it once. The context keeps ordering the write as
    above, and the graph resource becomes graph-managed through `Builds::Change`, which runs the change at the next poll with no
    build running and builds every epoch again. No graph change when nothing looked it up. Logged as
    `[ORG native] <name>: an exempt texture written after creation; promoted to tracked`.
- **Tracked:** everything else, once the render graph is up.
  - A persistent `org::PixelBuffer` (materialized, never aliased), named `cs.native.texture.<serial>`. ORG makes it Undefined.
  - The record imports its image for the context and the D3D11 views.
  - Home states are still on, and the context's raw operations still expect Common. So the texture is moved to Common at creation
    (with its contents), its tracker says Common, and `org::Resource::SetHostHome` makes each execution give it back in Common.
  - Kept host-made (a BasicRHI image the graph imports, as before), logged once per reason as `kept host-made, not graph-owned`:
    textures made before the graph is up, 1D textures, misc flags other than a cube, swizzled formats, D3D12 typeless depth, depth
    textures that are not one 2D slice, and sRGB textures with a UAV.
- **No Common-only imports in native mode.** `ImportDxvkImage`/`ImportDxvkTexture` no longer use simultaneous access for images
  resting in GENERAL. The graph moves every native image into each pass's layout and back to its home, as D3D12 already did.

**Lookup.** `Native::GraphTextureOf(object)` (and `GraphTextureOfImage(image)` for DCLF) returns the tracked texture's
`PixelBuffer`, or null (exempt, host-made, or not native's). `FrameAnchors::Import` and DCLF's `ImportImage` call it first in native
mode. Until the imports hold a `GloballyIndexedResource` (lane `p2-imports`: `NativeBacking` takes this lookup), a `PixelBuffer` does
not fit their `ExternalTextureResource` type, so they still import the image from its description. That second resource is safe
while home states are on: both rest in Common between executions.

**ORG** (`org-native/p2-native-owned`, a6310fd):
- `RenderGraph::AddResource` accepts an exempt resource.
- Admission plans no barriers, seeds or commits for it, and keeps no ledger entry (`PreparedBackingState::exempt`).
- A pass that declares a write to it is refused at declaration.
- `Resource::SetHostHome`: a graph-owned resource that goes home under external home states.

**Checks for a native run:**
- `CS_ORG_NATIVE_VALIDATION=1`: no new layout errors. The likely ones are an exempt image used in GENERAL, or a tracked image read
  by an undeclared raw view during an execution.
- `CS_ORG_NATIVE_WRITE_CENSUS=1` with the promotion lines: the loading screen's texture should be the only one promoted.
- The `kept host-made` lines: what stays host-made and why.
- `CS_ORG_NATIVE_OWNED=0` against the default: the frame-rate change from dropping Common-only imports.

## Phase 2 item 2 lane: imports and engine targets as versioned slots (`org-native/p2-imports`)

**What versions now:**
- **Imports.** `FrameAnchors::EngineTarget/EngineDepth/EngineCubemap/D3D11Texture/ImmutableTexture/ImportDepth` return a
  `FrameAnchors::Texture`. It holds one `org::VersionedTexture` slot per import id, for the id's life.
  - A new D3D11 texture with the same format, mip count and slice count is published as the slot's next version, with
    `NoteNewVersions` and no build.
  - The old D3D11 texture goes through `ReleaseAfterD3D11`. The old version keeps its graph resource while a frame holds it.
  - Each anchor graph registers the slot as the id's resolver.
- **Engine targets** (`CS_ORG_ENGINE_TARGETS`). `EngineTexture` is a versioned twin, so a target made again in the same shape
  class (a resize, `Recreate` of the same format) is a version. "Engine::kMAIN" and the other ids are published as the slot.
  `Targets::Find/FindImage` return the slot.
- **Drawcall Limit Fix.** `ImportImage` returns the engine target's slot when ORG owns the image. Otherwise it returns the anchors'
  import of the same image (`FrameAnchors::ImportOfImage`). Only when neither exists does it import the image itself.
  - Its passes declare the slot, and `Register` registers the slot as the id's resolver.
  - The direct sites keep the backing current when their views were made (`Texture::Current()`). A new version therefore makes new
    views, and the old image lives as long as its views do.
- **One handle for passes.** `FrameAnchors::Texture` goes straight to the pass builder (`a_builder.ShaderResource(texture)`). ORG's
  new `SelfDeclaringResource` hook (ORG `org-native/p2-imports`, 6f69e83) then declares the slot. A plain `shared_ptr` resource
  converts to a plain handle, which declares itself. `AnchorPasses::Bind::Srv/Uav`, `Draw::target`, `Copy`, `Clear`, `Upload` and
  `GraphPasses::MakeUavClear` take the handle.
  - Shape queries go through `Width()/Height()/Format()`.
  - Raw handles outside the graph come from `Current()`, which must never be declared.
- **Bindless indices** come from the declared view (`a_recording.Index(token)`), which resolves the version the preparation binds.
  `PublishedResources::ShaderResourceIndex` reads the slot's current version.

**Size changes.** A version of another size is still a version, but it also asks the build manager for one async pass rebuild:
anchor epochs for an import, every epoch for engine targets (`frame anchors: import '...' resized`, `engine targets: ... resized`).
This is not a structural rebuild. It remakes passes that read a size when they were made (Deferred clears, the Skeleton's copy,
Volumetric Lighting's viewport). Until the attach, those passes run with the old size on the new version.

**Still structural, and why** (all through `GraphResources::RequestStructuralRebuild`, so through the build manager, off the render
thread):
- **A change of shape class.** These are HDR's LDR target upgrades and Deferred's water targets: a format change, five at setup in
  the last native run. Each makes a new slot. The anchors also see the resize generation once.
- **A brand new import id** (`RequestFor`, only the graphs that looked it up).
- **`ReleaseImport`.**

**Clients that still rebuild on their own** (pass rebuilds, not structural): clients that compare the D3D11 texture rather than the
handle. These are the Skeleton's clears, ImageSpace's `EngineObjectOf`, Scaleform's surfaces and DCLF's own image check
(`targetTexture`). Bink now refreshes only when the slot or format changes.

**What to check in a run:** `[ORG] structural rebuild` lines on a load (expect only the shape-class ones at setup), the
`... resized` build-manager reasons on a resolution change, `[ORG] declared view mismatch` (the build-time latch in
`DeclarePublishedRead` can log once per id after a version), and shots against the baselines.

## Phase 2 results (2026-11-13, lanes p2-twins-a/b, p2-buffers, p2-native-owned, p2-imports merged)

ORG is 48c5276: exempt resources accepted (a6310fd) merged with the `SelfDeclaringResource` hook (6f69e83).

Guest and native quick shots match the earlier runs. The native tour passes. No graph or epoch failed.

**Imports and twins.**
- Imports, engine targets and DCLF's import of those images are `FrameAnchors::Texture` slots.
- The twins lanes' clear and copy helpers build the same handle (`AnchorPasses::TextureOf`).
- Converted twins: Screen Space Shadows, SSGI and Exponential Height Fog.
  - The fog's per-frame volumes are no longer aliased: roughly 30-50 MB.
- A native load still logs 6 structural rebuilds, all through the build manager, none on the render thread:
  - Five are format changes of engine targets at setup: kLDR_DOWNSAMPLE0, kLDR_BLURSWAP, kIMAGESPACE_TEMP_COPY, kWATER_1 and kWATER_2. Their first version has another shape class.
  - One is "graph resources resized" at the first world frame, from a twin that is not versioned. It rebuilds 25 epochs, attached 10 frames later.
- The tour's loads, `coc` and menus add no structural rebuild.
- The 19 graphs built on the render thread in the tour are all DCLF's epochs. They need copy-on-write DCLF state.

**Buffers.**
- The native census's world window reads 0 undeclared accesses.
- The loading screen still has 11 sites, mostly DCLF table buffers referenced by handle at capture, plus Scaleform and one ring acquire.

**Native ownership** (`CS_ORG_NATIVE_OWNED`, now registered with the test harness).
- Exempt textures and graph-owned tracked textures are made.
- The graph-owned lookup is not wired into imports yet. Imports still import the image.
- **Exclusive native imports are off again** (d34e21c5f). The anchors and DCLF imported the same engine image as two graph resources, and their exclusive layout moves raced, which made post-processing flash. With p2-imports, most of these share one slot, but some images still have two resources:
  - an image DCLF imports before any anchor client does;
  - Scaleform's own import of HDR's UI texture, which Bink imports as `hdr.ui`;
  - ImGui's import of a target that is not the framebuffer.

**Next for Phase 2:**
- Make every image one graph resource, then wire `Native::GraphTextureOf` into `ImportBacking` and turn exclusive native imports back on.
- Make the five engine targets in their final format from the start.
- Find the twin behind "graph resources resized".
- Declare the loading-screen sites.

## Phase 2 item 2 lane: engine targets in their final format (`org-native/p2-target-formats`)

**The six rebuilds on a load.** HDR Display's LDR upgrade and Deferred's water targets remade engine targets in another format
after `EngineTargets::TakeOver` had made and published them. ImageSpace and the Skeleton had already looked those ids up, so each
format change made a new slot:
- `published resource ... replaced by a new resource` for kLDR_DOWNSAMPLE0, kLDR_BLURSWAP, kIMAGESPACE_TEMP_COPY, kWATER_1 and kWATER_2. kIMAGESPACE_TEMP_COPY2 and the two TAA UI targets changed format too, but no reader held them yet.
- **Rebuild 6** came from the same five. `Settle` bumped the resize generation for each replaced held image. The anchors only see the generation at their first run, which is the first world frame, so they logged one "graph resources resized" rebuild there. No twin resized.

**What changed:**
- **Plans.** `Targets::PlanTarget(target, Plan)` is called before the graph exists:
  - HDR's `PostPostLoad` plans its seven LDR targets as R16G16B16A16_FLOAT.
  - `Deferred::PlanTargets` (from its hook install) plans the GBuffer and water targets from the same table `SetupResources` uses: format, RTV and SRV binds, kMAIN's size, no copy.
- **TakeOver and every later engine Create** make a planned target in its planned shape (`PlannedShape`), so the first version already has its final shape class. It logs `made in format ... from the start, as <owner> remakes it`.
- **The owners' upgrades find the right shape.** HDR's upgrade sees the HDR format and skips the target, so it saves nothing and has nothing to restore. Deferred's `Recreate` finds the same shape and does nothing (`already made as ... wants it`). `Restore` goes back to the planned shape.
- **Off or on D3D11.** Without `CS_ORG_ENGINE_TARGETS`, without HDR loaded, or for a target the engine keeps on D3D11, plans do nothing and the old paths run. A `Recreate` to another shape at runtime is still a new slot, through the build manager.
- **Named bumps.** `BumpResizeGeneration(who)` records what moved the generation. The anchors' line now reads `graph resources resized by <names> (...)`. EngineTargets' bump is named `engine targets (<target>)`; a twin's resize is named after the twin.

**What to check in a run** (native and guest): no `[ORG] structural rebuild` on a load, and no `not a new version` warning for engine targets. Look for the `made in format 10 from the start` lines for the planned targets, and `already made as Deferred::Water1 wants it`. If a "graph resources resized" rebuild remains, its line names the cause. Compare shots against the baselines; post-processing and water should look the same, since the final formats are unchanged.

## Phase 2 lane: loading-screen declarations (`org-native/p2-loading-decl`)

The native census's loading-screen window listed 11 sites. Most were refs and bindings made once and then reused. The validator
counts a ref when it is made, not each time a shader reads it. So a ref made during a load shows only in the loading-screen window,
and the world window reads 0 even when nothing declares the read. Each site, with its verdict:

| Site                                                          | Verdict                                                                                                                                                                                                           |
|---------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| EngineDrawFamily.cpp:336 (`GpuResources::Acquire`, the ring)  | False positive. The menu scenes' pass declares the ring (`DeclareRingRead`). `Acquire` no longer notes an ORG-owned buffer by handle; `Engine::Buffers::Resolve` judges it where it makes the ref.                  |
| GrassTables.cpp:272, TreeLodTables.cpp:62                     | Declared already. The draw rows are declared by the main, cull and arguments passes. The refs come from `DeclaredRefOf` now. The versioned tables' refs there only check that a ref can be made.                   |
| FacePositions.cpp:63                                          | Only a check that a ref can be made; readers use `DCLF::RefOf`. `DeclaredRefOf` now.                                                                                                                              |
| Resources.cpp:480, 802, 874, 895                              | Declared already: the main and view frame constants, the shadow constants, the faces' rows (passes named beside each call). `DeclaredRefOf` now; the foliage record's refs too.                                     |
| TextureFeedback.cpp:34                                        | Not a graph access. Host-visible feedback words that the colour pass writes with no order and only the CPU reads. A new validator rule, `NoteHostFeedback`, counts its refs as "host feedback".                       |
| FrameRecords.cpp:242 (t70, `kPRECIPITATION_OCCLUSION_MAP`)    | Real. Wetness Effects binds the engine's map at t70 for every colour draw, and the commit imported it again. The colour passes now declare `Engine::Depth::kPRECIPITATION_OCCLUSION_MAP`, and the commit reads its index. |

**The t70 path** (`Extensions::PrecipitationOcclusionTextureIndex`):
- It takes the target through `FrameAnchors::EngineDepth`.
- If ORG owns the target, `Engine::Targets` publishes the id when the import asks for it.
- Otherwise the anchors' import is published under the same id, with the bound view as its export.
- The first publish builds every graph again, once, through the build manager. Until then the read stays the D3D11 import.
- Another view bound at t70 is imported as before.

`DeclaredRefOf` has a device overload for setups that have the device before the graph host. Tree LOD's debug stamps
(`CS_DCLF_TREE_LOD_STAGES`) keep a raw ref: the buffer has no UAV flag, and no pass declares it.

**For the main session:**
- Native census: the loading-screen window should list no site but Scaleform's `Passes.cpp:508`. The summary line has a new
  "host feedback" count.
- Look for `[DCLF] frame texture precipitation occlusion (t70) reads the graph resource 'Engine::Depth::kPRECIPITATION_OCCLUSION_MAP'`.
  `... reads its D3D11 view` that lasts past the first world frame means the export did not match the bound view.
- Guest: the same line, with `[ORG] 'Engine::Depth::kPRECIPITATION_OCCLUSION_MAP' published to every epoch's graph` when engine
  targets are off. Wetness Effects' puddles and rain occlusion should look unchanged in shots.

## Phase 2 items 1-2 lane: one graph resource per image (`org-native/p2-one-resource`)

**One registry.** FrameAnchors' imports are the only way an engine or feature image becomes a graph resource for the anchors, DCLF,
Scaleform and ImGui. Every import finds the image's slot (by its VkImage) before making one:
- A second id of an image shares the first id's slot (logged `... is the image <id> has already; it shares that slot`). This replaces
  the old warning about two ids on one image.
- An importer that needs a view the slot lacks gets a new version of the same image with both sets of views. Passes keep declaring the
  slot, so nothing is built again. A view both ask for in different formats keeps the slot's (logged once per kind).
- `FrameAnchors::ShareImage` is DCLF's way in (`ImportImage` now takes the D3D11 texture). Before any id import of the image, it makes a
  loose slot. The slot's versions keep the D3D11 texture alive, so the image handle is never reused while the slot lives. An id import
  of the same image later takes the slot over. `ImportOfImage` is gone.
- `FrameAnchors::ImportTexture(id, texture, views)` covers targets with no D3D11 views to describe them. Scaleform's and ImGui's own
  imports use it; HDR's UI texture goes through `D3D11Texture("hdr.ui", ...)` with Bink's views, so all three share one slot.
- Every importer describes subresources the same way (all mips, cube slices from the D3D11 flags), so the shape class matches.

**Native lookup.** In native mode, `MakeBacking` takes the image's graph-owned `PixelBuffer` (`Native::GraphTextureOf`) as the slot's
version when the texture has every view asked for. Otherwise it imports the image with the texture's views and the asked ones, and the
`PixelBuffer` is never handed to a graph while that import stands. `VersionedTexture` image versions take any `GloballyIndexedResource`,
and `Width/Height/Format` already read a `PixelBuffer`. The `PixelBuffer` keeps its host home (Common), as the imports do, so the
context's raw GENERAL operations and the graph agree while home states are on. `ImmutableTexture`'s native exempt path, dropped by the
p2-imports merge, is back.

**Exclusive native imports.** `ImportDxvkTexture` imports exclusively in native mode again (with `CS_ORG_NATIVE_OWNED`). Raw imports
(`ImportDxvkImage`: the native context's own import, DCLF's bindless views) stay shared in GENERAL, where they are used between
executions. Guest mode is unchanged. `CS_ORG_NATIVE_EXCLUSIVE_IMPORTS=0` keeps native imports shared.

**Scaleform's loading-screen reads.** In native mode, the tracked textures the displays read are declared by Scaleform's passes.
These are graph-owned textures and exempt ones promoted after a write (`Native::PromotedTextureOf`). A draw binds the declared view's
index in place of the raw one. A texture seen for the first time registers the client again after the run. Until that build is
attached, it keeps the raw index, and the display is replayed as after a surface was made again.

**Duplicate check.** `CS_ORG_IMAGE_DUPLICATE_CHECK=1` notes every graph resource of an image, from `ImportDxvkTexture` and from native
mode's graph-owned and exempt textures. It logs `[ORG] duplicate image: '<a>' and '<b>' are two live graph resources of image 0x...`
when a second live one appears. A slot's replaced version is retired first, so views added to a slot do not count.

**Checks for a native run:**
- The flashing test: a native quick shot against earlier runs, and watch post-processing for flashing.
- `CS_ORG_IMAGE_DUPLICATE_CHECK=1`: expect no `duplicate image` lines. Lines that remain name what still imports an image on its own
  (GraphResources twins whose D3D11 export is imported again, engine textures, streaming windows).
- `CS_ORG_NATIVE_VALIDATION=1`: no new layout errors. A raw bindless view (GENERAL) read inside an execution that moved the image
  elsewhere is the likely one.
- `frame anchors: ... slot has the views ... asks for now` and `... in another format` lines, and the Scaleform `tracked textures read by the displays` line.
- `CS_ORG_NATIVE_EXCLUSIVE_IMPORTS=0` against the default: the frame-rate change from exclusive imports.

## Phase 2 items 1-2 lane: no duplicate images (`org-native/p2-duplicates`)

**What the three duplicates were.** None was the native `PixelBuffer`. Native mode forces `CS_ORG_ENGINE_TEXTURES` on, so:
- The flow map (`Engine::Texture2D 320x320`, made by ImageSpace through `Renderer::CreateTexture2D`) and VL's noise gradient
  (`Engine::Texture2D 8x8`) are engine textures. `Engine::Textures::Create` wraps a BasicRHI image and imports it as a graph resource.
  The anchors then imported the same image again. The noise gradient already went through `ImmutableTexture`, but a wrapped image is
  not native's exempt class, so it fell through to `D3D11Texture`.
- `cs.engine.depth.10` held Skylighting's twin export while Skylighting swaps it into the engine's field (DCLF's engine draws import
  the field then).

**Made images.** `RenderGraph::RegisterMadeImage` (in `DxvkImageImport`) maps an image ORG made to its one graph resource and the
views it has:
- `GraphResources::ExportTexture`: every twin export and engine target. A versioned twin also registers its slot (`AdoptSlot`).
- `Engine::Textures::Create`, texture streaming windows, and native exempt textures (`ImmutableTextureOf`).

`FrameAnchors::Import` (every id import) and `ShareImage` (DCLF, Scaleform) ask `MadeImageOf` before anything else but ORG's engine
targets. A hit returns the twin's slot, or the resource as a plain `FrameAnchors::Texture`. The id is not registered for it. A made
resource that lacks a view the importer asks for falls back to an import, logged as
`frame anchors: ... asks for a view '...' lacks`. A read-only native import calls `ImmutableTextureOf` first, so an exempt texture
keeps its one resource whoever asks first. Makers whose import does not own the image (engine textures, windows, native records)
call `ForgetMadeImage` before the image can go.

**Graph-owned textures are always the backing.** `MakeBacking` no longer re-imports a graph-owned image when an importer asks for a
view it lacks. The `PixelBuffer` has every view its bind flags allow, so the only possible mismatch is a format, and a re-import kept
the `PixelBuffer`'s formats anyway. `EnsureViews` leaves a `PixelBuffer` slot alone. Logged as `the graph-owned texture has the views
asked for in other formats`.

**The t70 site** skips a twin export in the precipitation map's field. Published under the engine's id, the twin would replace the
engine map's slot each time the two took turns.

**Scaleform's loading-screen site.** The census's `Native::Texture2D` was a host-made native texture: neither graph-owned nor a
promoted exempt one. `DeclaredRead` now declares every tracked texture (`Native::IsTrackedTexture`: graph-owned, host-made or
wrapped) through `ShareImage`. The raw `ResolveBinding` for a read declared before the commit runs in a new
`UndeclaredAccess::DeclaredViewScope`, which counts it as "declared (interim index)". Native textures with `GENERATE_MIPS` can be
graph-owned now. `MakeSurface`'s unused state parameter fed a counter the previous lane removed; it is gone.

**Duplicate check.** `ExportTexture`'s non-GENERAL branch (guest mode, D3D12) now notes its resource too.

**Exclusive native imports are the default again** (a separate last commit, which the main session can drop).

**For the main session:**
- Native quick run with `CS_ORG_IMAGE_DUPLICATE_CHECK=1`: no `duplicate image` lines. A `frame anchors: ... is the image of '...',
  which ORG made` line for the flow map, the noise gradient and the occlusion twin.
- Haze and fog in the native frame with exclusive imports (the regression that made shared the default).
- Native census, loading screen: no `Passes.cpp` Scaleform site; its reads show in the summary's interim count.
- Guest with `CS_ORG_ENGINE_TEXTURES=1`: the same `is the image of` lines, shots unchanged (water flow, VL, Skylighting).
- Watch for `asks for a view ... lacks` and `graph-owned texture lacks a view` warnings: each names an importer the registry did not
  fit.

## Phase 2 done (2026-11-13, integrate e02bd7995)

All Phase 2 lanes are merged. Native and guest loads log 0 structural rebuilds and 0 duplicate images
(`CS_ORG_IMAGE_DUPLICATE_CHECK=1`). The native census reads 0 undeclared accesses in the world; the loading screen has one Scaleform
read in its first window. Shots match in both modes.

**Exclusive native imports moved to Phase 4.** With every image one graph resource, exclusive imports
(`CS_ORG_NATIVE_EXCLUSIVE_IMPORTS=1`) still flash post-processing and lose the haze. What is left must be work outside the graph's
state tracking on the same images: the native context's raw operations in GENERAL, and DCLF's bindless views written as GENERAL
descriptors. Phase 4 turns home states off and replaces the context's raw operations with passes and `ExternalClient`; exclusive
imports become the default there, and only once `CS_ORG_NATIVE_VALIDATION=1` and a normal-speed run both show no layout race.
Shared imports (Common-only, simultaneous access) are the default until then.

## Phase 3 lane: the engine's leftover operations (`org-native/p3-static-ops`)

The native op census's world window had four kinds of GPU work left on the immediate context, besides the CPU-only constant
buffer maps: the clears of `kTEMPORAL_AA_MASK`, `kWATER_1` and `kWATER_2`, and the copy of `kMAIN` into "kMAIN Copy" (its
`textureCopy`). Loading screens add uploads into one texture and a few copies.

**Static passes in the engine skeleton.** The skeleton already has one gated clear pass per engine view and a copy at an anchor
of its own. Native mode now adds:
- Three views to the frame group, in native mode only: `kTEMPORAL_AA_MASK`, `kWATER_1` and `kWATER_2`. Each is one pass at
  EngineFrameClear, opened by the group's run gate with its colour latched, like the skeleton's other clears.
- A second copy beside the depth copy: `kMAIN` into its copy texture, at a new anchor, **EngineCopy** (native mode only). Like the
  depth copy it has no gate: a copy has no indirect form, and the anchor only ever runs for that copy. The copy texture is the
  published `Engine::kMAIN Copy` slot (`Targets::Find`), so a resize is a new version.
- If these clears reach the skeleton's patched sites (`ApplyState`, `SetDirtyStates`), the sites now take them. If they reach the
  native context directly, the context hands them over.

**The native context asks first.** `ContextClearRenderTargetView`, `ContextClearDepthStencilView` and `ContextCopyResource` call
`Skeleton::Sites::TakeContextColourClear`, `TakeContextDepthClear` and `TakeContextCopy`.
- A taken operation runs on the graph at once (the group's anchor, or EngineCopy), at its place in the stream.
- A run that fails replays the clears through the engine's context. The skeleton is marked as flushing then, so the native context
  records them itself instead of handing them back.
- Never inside an anchor's run: `FrameAnchors::InRun()` is new for that.

**Rare work through ORG's external client.** `RenderGraph::ExternalWork` wraps one `org::ExternalClient` on the host's runtime,
with the graphs' queue slots. It is made on first use.
- Native `UpdateSubresource` (textures), `CopyResource` and `CopySubresourceRegion` between images the graph tracks go through it,
  with their accesses declared (copy destination, copy source; whole resources). The barriers come from the same ledgers, and no
  graph changes.
- "Tracked" means a graph-owned `PixelBuffer`, or an exempt texture a write promoted once it is graph-managed. The loading
  screen's texture is the second kind, so its updates go through the client once the build manager has attached its promotion.
  Before that they keep the raw path, as before.
- The work records on the graph resource's own handle, the one its barriers name, not on the context's import of the image.
- While home states are on, the client's submissions are closed: a full barrier at entry, and every texture back in its home
  state at exit (Common for graph-owned textures, shader-read for promoted ones). That is the contract the raw path still relies
  on. Phase 4 turns it off with home states.

**Fallback stays.** Everything else keeps the context's own path: host-made images, buffers, staging textures, DYNAMIC uploads
(their upload memory is waited for by the context's own timeline), mip generation, UAV clears, and anything a static pass or the
client refuses.
- Each such operation is logged once per (operation, resource) while the render graph is active, up to 64 lines:
  `[ORG native] <operation> on "<name>" runs on the immediate context's own path ...`.
- `Immediate`'s raw GENERAL path and the RHI path's Common transitions are not deleted. The loading screen, uploads into
  host-made images and the CPU-side staging work still reach them.

**Op census.** Routed operations are no longer counted as immediate operations. A new line per window counts them apart:
`[ORG native] op census (<window>): not on the immediate context (a frame): graph passes: ...; external work: ...`.

**For the main session** (native census run, `tools/cs-test/configs/native-census.toml`):
- World window: the by-kind line should show only `Map write (discard)` (about 240 a frame). The new line should show
  `graph passes: ClearRenderTargetView ~3, CopyResource into ~1, CopyResource from ~1`, or fewer if the skeleton's sites took the
  clears before the context saw them.
- No `[ORG native] ClearRenderTargetView on "Engine::kTEMPORAL_AA_MASK" ... own path` line (nor for the water targets or
  `kMAIN Copy`) after the first world frame.
- `[ORG] engine skeleton: the frame clears registered at the EngineFrameClear anchor (N views ...)`: N is three more than in guest.
- `[ORG] engine skeleton: the kMAIN copy (kMAIN into kMAIN Copy) runs on the graph (EngineCopy anchor)`.
- Loading screen window: `external work: UpdateSubresource texture ...` once the texture's promotion is attached, and
  `[ORG] external work: ORG's external client is ready (closed submissions ...)`.
- Warnings to watch: `the kMAIN copy ...: the textures differ in size or format` (the copy stays raw), `stays on D3D11 (an integer
  format)` for the TAA mask, `external work: '...' could not be submitted`.
- Guest: unchanged. The skeleton's extra views and EngineCopy exist only with `CS_ORG_NATIVE=1`; its flush code was only split
  per group.
- Shots: the TAA (the mask) and water must look the same; a wrong copy shows as stale refraction in water.

**Risks.**
- The clears now run as graph draws inside an anchor run that starts from the native context's call. That is new: until now, every
  anchor run started from an engine or CS hook. A D3D11 call made inside another graph epoch's body (not through
  `FrameAnchors::Run`) is not guarded.
- ExternalClient's closed submissions are a full barrier each. That is fine for rare work. If the census shows many external
  submissions a frame in the world, they should become passes.
- A promoted exempt texture changes to graph-managed through the build manager on the render thread, while the loading thread may
  read the flag. The race is benign: either answer picks a working path.

## Phase 4 item 4 lane: DCLF's epochs off the render thread (`org-native/p4-dclf-async`)

DCLF's epoch graphs are no longer built on the render thread. The scene worker builds them from an immutable **build state**, and the build
manager attaches them at a frame's end. `CS_DCLF_ASYNC_BUILDS=0` restores the old path (built on the render thread at the build point).

**The build state** (`Core/Render/Epochs/BuildState.{h,cpp}`).
- One immutable value holds everything an epoch build reads that the render thread changes:
  - the requested graph conditions (all of them, not just the three bits the revision inputs carried);
  - the main resources and the views drawing from them (`Resources::views`);
  - each view's resources, target, DCLF depth, size and main resources (`ViewBuild`);
  - the shadow resources and `ShadowSlotBuffers`, which now also hold the slot owners, the count buffers and the shadow maps with their
    layer counts;
  - the pass setup states.
- The render thread makes it (`Impl::CurrentBuildState`) and compares it with the last value. A new value gets a new **serial**. It is
  posted with the revision inputs (`RevisionInputs::build`).
- The factories take it with `BuildingState()`. They no longer capture live resources, so `AddExtension` keeps working only as the rebuild
  request.
- The makes read it through `PassContext::build`. The view passes (`ViewEpochRecord.cpp`), the shadow passes (`ShadowEpochPasses.cpp`,
  `Shadow.cpp`), the sites (`SitePasses.cpp`), the hosted views and pass states in `GatherScheduled`, and tree LOD's faces all read the
  build's targets, slots and main resources. They never read the live vectors and pointers the render thread replaces.
- Per-frame fields stay on the live resources: latched targets, labels, frame buffers. Only preparation and recording read them.
- `BuildScope` is an RAII guard that sets the calling thread's build state. A build without a scope logs:
  - on the render thread (the host's first build of an epoch at its first submission) it is an info line, and the build uses the newest
    value;
  - anywhere else it is an error, and the build uses the attached value.

**Conditions.** `RefreshGraphConditions` sets the *requested* conditions. The frame keeps the *attached* ones until the builds attach
(`PublishAttachedBuildState`). Outside a build, `CurrentGraphConditions` returns the attached conditions, so the frame's mode
(`GraphBuiltForCpu`), the preparations and the revision inputs follow the graph that runs. Inside a `BuildScope` it returns the build's
conditions. Every attached value is kept alive, so the reference stays valid while another thread swaps it.

**The builds** (`Core/Render/Epochs/GraphBuilds.cpp`, `BuildManager::ExternalBuilder`).
- DCLF's epochs are the build manager's *external* epochs. At each poll, the manager hands those that were asked for to the "DCLF graph
  builds" lane on the scene worker's preparation pool. Each build runs inside a `BuildScope` holding the build state made at that poll.
- Policy stays with the manager:
  - one group per poll;
  - a request made since a build was handed out supersedes it;
  - an `AddExtension` since then rejects it at the attach (the epoch's configuration moved);
  - a failure keeps the old graph;
  - the per-attach line: `[ORG] build manager: <epoch> attached: built in X ms off the render thread, attached in Y ms ...`.
- External groups attach only at `Attach::All` (`SealPresented`, every frame's end, after DCLF's epochs were submitted), or at
  `Attach::External` (DCLF's own wait for a snapshot at a frame's start, before it submits anything).
- After each such poll, DCLF settles (`SettleBuildState`). If none of its epochs is asked for or in flight (`BuildManager::Settled`), the
  newest build state becomes the attached one. The revision inputs are then posted again and the snapshot builder is woken.
- Every build the manager makes on the render thread (shader reload, setup, `BuildOwnerEpochsNow`) holds a scope with the render thread's
  newest value (`ExternalBuilder::ownerScope`).

**Recordings.** Recordings are made after the attach. A detached graph cannot be recorded:
- `RequestEpochRecording` records the instance's attached graph on the host's thread, with its copies and tickets.
- Its alias placement waits for the first prepare after the attach (ORG c8c5086).
- The transient resource objects are shared by every graph of an epoch, because the extensions register the same objects each build. So
  giving a detached graph its own arena range would still re-materialize objects the running graph binds. Separate per-graph transients
  would be a deep ORG change, and none was made.

So:
- `MakeRevision` makes nothing while its inputs' build serial is newer than the attached one. A snapshot made from a newer serial is stale
  (`StaleKindOf`: `kStaleBuild`). Frames keep the old snapshot and graphs meanwhile.
- The attach at the frame's end moves DCLF's build generation, as a synchronous build did. The host's thread places and records the new
  graphs while the engine updates the next frame.
- The next frame's start adopts the new snapshot through the existing staleness wait. Each such frame logs one of:
  - `[DCLF] frame N waited X ms at its start for the recordings of the graphs attached at the last frame's end`;
  - `... adopted a snapshot of the graphs attached at the last frame's end without waiting`.

**Still synchronous.**
- **Main resources made again under claims** (`MainEpochs.cpp`): the frame's colour epoch draws its own preparation with them, so DCLF's
  epochs are still built there (logged as synchronous). A scene-worker build of them that is in flight is rejected at its attach. This is
  rare: a resize, or the frame buffers' shape changing.
- **Shader reload** builds every graph on the owner thread with the host's thread held (`RequestAllSynchronous`). The pipeline registry's
  publish needs that safe point, and a managed build never publishes. Moving it off needs an ORG hook that publishes inside the attach's
  hold. DCLF's epochs there build from the render thread's newest build state.

**What to check.**
- A native tour logs 0 `graph built on the render thread` warnings, and DCLF shots are unchanged in native and guest.
- One `attached: built in X ms off the render thread` line per DCLF epoch at each cell change.
- The `waited X ms ... for the recordings of the graphs attached` lines, which give the worst case.
- `[DCLF] graph builds: on the scene worker` once, and `[ORG] build manager: external builder set`.
- No `a graph build without a build scope off the render thread` error lines.

**Risks.**
- **What the makes still read live:**
  - the main `Resources` object's fields (made before it is published, then only `depthCommitted` and the `views` weak pointers change,
    and the build reads the views from its own state);
  - the features' registries (`Extensions::RegisteredFrameGraphBuffers`, `DeclareGraphTextures` id lists, the draw families);
  - `Diagnostics::ScheduleDumped`.

  The anchor builds already read these off the render thread, so they are assumed safe.
- **The scene worker's pool.** A build blocks its worker while ORG compiles pipelines on the runtime's task service. This is fine unless
  the preparation pool has a single worker that a pass declaration waits on.
- **`buildMutex` waits.** It is held around every managed build. A render-thread `Change`, shader reload or setup build waits for a
  scene-worker build in progress (3-8 ms).
- **Attached values that run ahead of their graph.** The attached value is the newest build state once nothing is asked for. A change
  that asks for no rebuild (the slot owners shared, a view set up, which never re-added the main extension before either) moves the
  serial without a graph. The graphs then run as built, as before this lane.

## Phase 3 and DCLF async builds (2026-11-13, merged; tested on the RTX 5080)

**Phase 3 is done.** The native census's world window reads only CPU-only constant-buffer maps (about 240 a frame).
- The `kTEMPORAL_AA_MASK` and `kWATER_1`/`kWATER_2` clears are three more views in the engine skeleton's frame clears.
- The `kMAIN` to `kMAIN Copy` copy runs at the EngineCopy anchor.
- Rare work on tracked textures goes through `ExternalWork`.
- The raw paths stay for host-made images, buffers, DYNAMIC uploads, GenerateMips and UAV clears. Each use is logged once.
- Shots match in both modes.

**DCLF's epoch graphs build on the scene worker** (`CS_DCLF_ASYNC_BUILDS`, on by default), from an immutable build state. They attach at a frame's end, and the next frame start adopts the snapshot.
- A native tour logs no "graph built on the render thread" warning, down from 18-19 with the switch off.
- It logs 16 DCLF attaches, each built in 0.4-4 ms off the render thread and attached in 0.1-0.9 ms.
- Two frame starts waited for recordings after an attach: 50 ms just after the first load and 28 ms at a `coc` into an interior (graph conditions changed). Both are at load transitions, none in play.
- Tour shots with the switch on and off match. Dragonsreach differs only where the NPCs stand.
- Still on the render thread: a rebuild of the main resources under claims (rare) and shader reload.

Test runs now use `CS_TEST_BACKGROUND=1`, so they never focus the game or move the mouse.

## Phase 4 item 3 lane: raw work and home states (`org-native/p4-raw-work`)

**What still touches tracked images outside the graph** (read from the code, before any run).
- **DCLF's bindless views: the likely cause of the flashing.** `GpuTextures::Import` imports a native image raw (`ImportDxvkImage`). A
  native image that rests in GENERAL gets simultaneous access, and BasicRHI then writes its view's descriptor with
  `VK_IMAGE_LAYOUT_GENERAL` (`generalLayoutOnly` in `rhi_vulkan.cpp`). With exclusive imports, the graph moves these images into
  `SHADER_READ_ONLY_OPTIMAL` in the pass that declares the read, so the descriptor names the wrong layout. This hits:
  - host-made images: the engine's targets are made at startup, before the graph is up;
  - GraphResources' native twin exports;
  - graph-owned PixelBuffers, which already move layouts even with shared imports.

  Every DCLF read of an engine target goes through these views, and so does every port that reads through
  `GpuTextures::ResolveBinding`'s interim indices. In guest mode the same images rest in read-only layouts, so the raw views name
  the read-only layout and match. That is why guest's exclusive imports work.
- **The native immediate context.** After Phase 3, the world's frames have no raw GPU work on images. What is left: loading-screen
  uploads and copies, DYNAMIC texture unmaps, mip generation, UAV clears, readback copies (the present probe, captures) and
  creation-time work. With home states on, these are correct between executions: every execution gives each image back in GENERAL.
  The exception is a call made inside an anchor run (Phase 3's risk).
- **The present's copy.** It assumes the back buffer in Common. That is right with home states on, and wrong without them.
- **Not covered.**
  - Buffers have no layouts, so they are left as they are.
  - Queue order: native mode has no exit queue boundary. A raw graphics-queue operation is not ordered after a compute- or
    copy-queue last use. With home states on, external homes release host-owned textures to graphics. Without them, the routed work
    takes its waits from the ledger. Raw buffer work is not covered either way.

**The diagnostic: `CS_ORG_RAW_ACCESS_CHECK=1`.**
- `[ORG raw access] '<operation> <name>' on '<name>' runs on the immediate context's own path and assumes GENERAL; the graph tracks
  the image as '<graph resource>' (<graph-owned | promoted exempt | host-made, imported>, <moved between layouts | Common only>,
  <goes home after each execution | no home>); <inside an anchor run | between anchor runs>: <RACE ... | safe while home states hold>`.
  Logged once per resource and operation. A race is any such use with home states off, inside an anchor run, or on a resource that
  moves layouts and does not go home.
- `[ORG raw access] DCLF's view of '<graph resource>' names GENERAL, but the graph moves the image between layouts; ...`. Logged once
  per image, while the graph is not moving native images. With it moving them, these views are made on the graph resource instead.
- Every 600 frames, when the counts changed: `[ORG raw access] so far: N native context operations on tracked images outside the
  graph (M where the graph may hold another layout); R sent through external work; G DCLF views in GENERAL of images the graph moves;
  V DCLF views made on the graph resource; home states on|off, exclusive imports on|off`.
- It sees only images. "Tracked" means an image with a graph resource (`GraphResourceOfImage`): a graph-owned texture, a made image,
  or the newest graph import of a host-made image.

**What changed.**
- **`GraphResourceOfImage`** (`DxvkImageImport`) finds the graph resource whose ledger follows an image. `ImportDxvkTexture` notes
  every native import for it.
- **`NativeGraphMovesImages()`** (native Vulkan, with home states off or exclusive imports) turns on the routing below. The default,
  shared imports with home states on, keeps the old paths.
- **The context's work goes through the ledgers.** `LedgerResourceOf` also returns a host-made image's graph import. `RouteRaw` sends
  an operation with at least one tracked image through `ExternalWork`. Each tracked image is declared in GENERAL (Common) with copy
  access, so the client moves it there from wherever the ledger says. The context's Vulkan commands stay as they were.
  - Covered: DYNAMIC unmaps, texture updates (depth too), copies with one tracked side (readbacks included), mip generation, and
    colour, UAV and depth clears.
  - Untracked images in the same operation keep the context's contract (exempt ones leave the shader-read state around it).
  - The work ends with a full barrier and signals the context's own timeline (`ExternalWork::Submit` takes extra signals), so `Map`
    and `Wait` still cover it. The context's lock is held around the submission, so its timeline values stay in order.
- **Copies between two tracked images** keep the client's copy-layout path (`CopyExternally`). A copy within one image now goes to
  `RouteRaw` (one access, reading and writing).
- **The present's copy** reads the back buffer through the client (`PresentThroughLedger`) when the graph moves native images.
- **DCLF's views** of an image the graph moves are made on the graph resource's own image (`ImportedBacking::graphResource`), so their
  descriptors name the read-only layout the declaring pass guarantees. Untracked images keep the raw import.
- **Home states off: `CS_ORG_NATIVE_HOME_STATES=0`.** Native Vulkan only; the default stays on.
  - `externalHomeStates` is off.
  - Exclusive native imports are implied, so no import is Common-only.
  - `SetHostHome` is off on graph-owned PixelBuffers.
  - `ExternalWork` becomes open, since it follows `externalHomeStates`.
  - Native in-stream epochs are off unless `CS_ORG_NATIVE_IN_STREAM=1`. DCLF's direct forward and first-person families need them, so
    they turn off (`GraphConditions`).
  - D3D12 and guest mode are unchanged.
- **Exclusive imports** stay off by default (`CS_ORG_NATIVE_EXCLUSIVE_IMPORTS`). With `=1` alone, home states stay on and the routing
  and views above apply.
- New log lines:
  - `[ORG] native home states: <on | off ...>; graph imports of native images: <exclusive | shared>`;
  - `[ORG] in-stream epochs: none without native home states ...`;
  - `[ORG] external work: ORG's external client is ready (open submissions ...)`.

**For the main session.**
1. A native quick run with `CS_ORG_RAW_ACCESS_CHECK=1` and the defaults: the races left with home states on. Expect no RACE lines
   between anchor runs. Expect `DCLF views in GENERAL of images the graph moves` above zero: those are PixelBuffers and twins.
2. The same with `CS_ORG_NATIVE_EXCLUSIVE_IMPORTS=1`: no flashing, haze present, shots match the shared run.
   `DCLF views made on the graph resource` above zero, `views in GENERAL` at zero.
3. The same with `CS_ORG_NATIVE_HOME_STATES=0`: the same visual checks. In the log:
   - the home-states line says off;
   - `sent through external work` above zero on loading screens;
   - no RACE lines (each one names raw work still on the context's own path).
4. `native-validation` with each of 2 and 3: no new layout errors. A descriptor-layout mismatch here would name a view still made raw.
5. If DCLF's forward or first-person draws differ with home states off, try `CS_ORG_NATIVE_IN_STREAM=1`.

**Risks.**
- ExternalWork submissions from inside an anchor run read the ledgers before that run's batches commit. ORG's prediction check
  (`Predicts`) should re-plan the run. This is not tested.
- Declaring copy access in GENERAL is a broad state for the ledger. The next graph use transitions out of GENERAL, which costs one
  barrier per routed operation.
- A DCLF read that some pass does not declare as a shader read sees a read-only descriptor on an image in another layout. The
  undeclared-access validator reads 0 in the world, so none is expected.
- With home states off, DCLF's direct forward families are off unless `CS_ORG_NATIVE_IN_STREAM=1`. First person may then fall to
  "the engine's" path (see `GraphConditions`' warning).
- Raw buffer work after compute-queue graph work is still unordered in native mode.

## Phase 4 items 1 and 2 lane: region subgraphs (`org-native/p4-regions`)

`CS_ORG_REGIONS=1` (native mode only, off by default) groups the frame anchors into six region epochs. Consecutive runs of a region's
anchors share one execution. Guest mode refuses the switch with a warning and keeps one epoch per anchor.

**Why not "submit at the region's last hook".** A region's anchors are interleaved with other GPU work: DCLF's epochs (shadow views,
the Z-prepass, reflection faces, main opaque, forward, first person), Light Limit Fix's culling, and anchors that run all over the frame.
Deferring an anchor's work past such work changes what it reads or overwrites. Examples:
- a shadow slice's clear must come before that slice's shadow-view draws;
- the main depth's clear must come before the Z-prepass;
- a reflection face's cube clear and its sky must come before that face's DCLF draws.

A static split list would have to know every such pair, and the run-order census shows the reflection faces alone in 43 sequences.
So the regions are split at run time instead:
- A region's execution is held open from its first anchor run.
- It is submitted just before any other GPU work.
- The GPU therefore sees every run's work in the same order as before. Only runs with nothing between them share an execution.

**How a run joins** (`FrameAnchors.cpp`: `Capture`, `JoinRefusal`, `SubmitHeld`).
- **Which anchors.** An anchor's passes move from its own epoch to its region's when every pass at it is gated (`PassDesc::gated`): each
  draw or dispatch goes through its client's run gate, or through a latch that reads zero in a run its commit did not write, and nothing
  clears by itself. A region execution holds the passes of all its merged anchors, so a pass whose anchor did not run in it must do
  nothing. The decision is made per snapshot (`Snapshot::merged`): a client that registers an ungated pass moves the anchor back to its
  own epoch at the next build. Both graphs are asked for whenever a client changes.
- **Gated passes.** `AnchorPasses::Builder` sets `gated` itself: a draw with a gate entry and no clear load, or a dispatch with a gate
  entry or latched group counts. ImageSpace's passes and the engine skeleton's clears set it by hand. One-shot passes, copies, uploads,
  UAV clears and hand-written passes without the flag keep their anchor in its own epoch.
- **The held run.** ORG's new `GraphHost::HoldEpochTicket` takes the region epoch's next ticket at the first run. Each run's commits
  write into that ticket's run (`UseHeldRun`). The next `SubmitEpoch` of the region submits it. Gates are closed once, when the hold
  begins, for every client of the region graph's merged anchors and for retired registrations.
- **Joining rules.** A run joins the held execution only when all of these hold; otherwise the execution is submitted first and the run
  starts a new one:
  - its anchor comes later in the region's order (the anchors' places in `kTimeline`), or it repeats the last anchor;
  - none of its active clients committed into the execution already, unless the client says it can (`ClientDesc::joins`);
  - a repeat has exactly the clients of the earlier run;
  - CS's shared constants are what the execution's first commit wrote (`SharedConstants::CommittedMatches`);
  - no `ExternalWork` submission happened since the hold began (it cannot be submitted after; it is logged once).

  So each latch region and gate entry is written once per execution, and a region graph's pass order (anchors in frame order, each
  anchor's passes by order key) is the order the runs happened.
- **Submitted before:** `TryExecuteEpoch` and `ExecuteExternalEpoch` of any other epoch, `EmitExternalCommands`, every build point
  (`GuardedBuild`: an attach would give the held ticket back), `FlushPendingStreamWork` (a render-thread GPU wait), the present (the
  present hook, just before the native present), a change of render owner, and `SealPresented` as a safety net. A run of an anchor with
  its own epoch, or of another region's anchor, submits it as well.
- **Executed callbacks** run at the capture. The held work reaches the GPU before any later GPU work, which is what `executed` promises.
  The exception is a region submission that fails later: its clients are dropped then.
- **Uploads** a commit queues ride the region's submission, which comes before any other submission.

**Repeated runs as static gated passes.** These anchors already have one gated pass per (operation, engine target):
- ImageSpace: one pass per shape;
- the skeleton's clears: one pass per view.

ImageSpace's runs are the scopes of the chain. Its client now joins a held execution when the batch's first entry is above the last
entry it committed. A batch's entries rise (`AdmitShape`), so no entry is opened twice and later passes run later. Its held heap-index
sources are kept per run instead of per lease index. Several commits into one run add to them; a new run clears them.

The skeleton's clear groups do not join: each run latches colours by entry, and two runs of a group could clear the same view.
Consecutive clears with nothing between them are one run already (the skeleton batches them).

**The region map as built** (`kRegionOf`):

| Region | Anchors | Splits that happen at run time |
|---|---|---|
| Shadows | EngineShadowClear, DecalQueries | every slice: the shadow view epoch follows each clear |
| Pre-main | EarlyPrepass, EngineFrameClear, EngineDepthCopy, ReflectionsPrepass, EngineCubeClear, SkyReflections, DepthDone, Prepass, Sky | Z-prepass, first-person depth, reflection faces, light culling, main opaque |
| Deferred | DeferredPasses, DeferredComposite, PostComposite | ungated clients (the composite, Dynamic Cubemaps' one-shot pass) |
| Engine draws | EngineDraws, EngineQueries | forward and first person; the families are not flagged yet |
| Post | VolumetricLighting, PostProcessing, Upscale, ImageSpace, EngineCopy, Tonemap, FrameGeneration | EngineCopy's ungated copy, ungated tonemap clients, changed shared constants |
| Interface | Interface, MovieTiles, Present, Overlay, HDROutput, Capture | DCLF's BeforePresent, ungated Scaleform, Bink, ImGui and capture |

Two departures from the brief's list:
- EarlyPrepass is in pre-main, not shadows. Its clients are the shadow features, but it runs after the sky occlusion epoch and right
  before the main depth's clear, which is pre-main's.
- An anchor that runs at several places of the frame (EngineFrameClear, EngineDepthCopy, EngineDraws, EngineQueries) has one region.
  A run elsewhere in the frame finds another region held and submits it first, so this is still correct.

The log line `[ORG] frame regions: ... anchors in their region's epoch: ...` names the anchors that actually merged.

**What merges, honestly.** Only consecutive runs without GPU work between them share an execution, and DCLF's epochs sit between most
anchors. The ~12 executions the plan hoped for need the anchors' work to move past DCLF's epochs, which this lane does not do. The gain
expected now:
- mostly ImageSpace's runs (about 12) collapsing into a few executions, split at EngineCopy;
- VolumetricLighting joining them when it runs right before;
- gated neighbours elsewhere, made with the Builder, when all their passes are gated: SSGI and Sun Rays in deferred, height fog and
  Skylighting at the prepass. Hand-written clients (Subsurface Scattering, Screen Space Shadows, IBL) keep their anchors' own epochs
  until they set the flag;
- the region epochs add up to six epochs, and merged anchors' own epochs run nothing.

Clients that could opt in later with `PassDesc::gated`: Scaleform, DCLF's engine-draw families, the sky, Bink, ImGui and the clients
with hand-written gated passes. That lets more neighbours merge.

**What is not hooked.** Two kinds of raw work run outside the anchors' reach:
- the native immediate context's raw operations (host-made images, UAV clears, GenerateMips, DYNAMIC uploads);
- `ExternalWork`.

Both live in the `p4-raw-work` lane's files. A region held across one of them would let that work run ahead of the held runs.
- `ExternalWork` is detected by its submission counter (logged once, and the held execution is submitted at the next run).
- The raw operations are not detected. The native census shows none in the world.

The fix is one call, `FrameAnchors::FlushRegions("raw work")`, at the start of `Immediate::Submit` and `ExternalWork::Submit`.

**Statistics** (with `CS_ORG_EPOCH_STATS=1`, every epoch-stats window):
- `[ORG] executions a frame: X over Y epochs` counts every submitted execution and the distinct epochs that ran. Read it with the switch
  on and off.
- `[ORG] frame regions: N executions held M anchor runs (R each); L lost to a build; submitted before: <cause> <count>, ...; anchors in
  their region's epoch: ...`. The causes show where regions split: another epoch, an anchor with its own epoch, out of order, a
  client's second commit, the shared constants, the present.

**ORG change** (`extern/OpenRenderGraph`, branch `org-native/p4-regions`, 53848cd): `GraphHost::HoldEpochTicket`, `HoldsEpochTicket`,
`UseHeldRun`/`EndHeldRun`, `ReleaseHeldEpochTicket`, and `CurrentRun()` returning the held run inside `UseHeldRun`.
- `SubmitEpoch` takes the held ticket.
- A build of the epoch (`HoldAsync`) gives it back first.
- `StopAsync` abandons it.
- persistent-epochs.md has the contract.

**What to check** (`tools/cs-test/configs/native-regions-quick.toml`, against `native-quick`; then a native tour with `--set org.regions=true`):
- The shots match with regions on and off: ImageSpace's chain (bloom, TAA, depth of field, water), the deferred features, the UI.
- `[ORG] frame regions (CS_ORG_REGIONS): ...` once at startup, naming the region epochs.
- The anchors-in-region list includes ImageSpace.
- `executions a frame` drops with the switch on; ImageSpace's share is the expected gain.
- No `lost to a build` above 0, no `held run back`, `GPU work was submitted from inside a commit` or `external work ... while a region
  held` lines, no `[E]` lines.
- Guest with `CS_ORG_REGIONS=1` logs the refusal and runs as before.

**Risks.**
- **Honesty of `gated`.** A pass marked gated that does work unopened would run at every execution of its region. The Builder's rule
  covers draws and dispatches. A draw with a clear load is not gated.
- **Executions per frame may not drop much** where shared constants change between ImageSpace scopes (the `the shared constants
  changed` cause shows it). Comparing only the blocks a run's passes read would help.
- **A build's attach while a region holds** gives the held run back. The build points submit first, so only a build outside them
  (shader reload's synchronous build is at a build point) would lose one frame's runs of that region (`lost to a build`).
- **Raw native work and `ExternalWork`** between two merged runs (above).
- **`executed` before the GPU submission.** A client that read back right after `Run` would need the region submitted first. None does
  today: readbacks go through ORG's readback service, which completes after the execution.

## Epoch statistics in native mode (`org-native/p4-stats`)

`CS_ORG_EPOCH_STATS=1` printed nothing in native mode. The runtime read the switch only in its guest (DXVK) setup branch, so native
mode never kept the statistics: no `executions a frame`, no `Run memory`, no `frame regions` line. The run-shapes lines print because
they have their own switch. The switch is now read for both modes.

Native mode reports every 300 presents, from `SealPresented`. Guest mode still reports every 600 epochs. Native's epochs a frame change
with `CS_ORG_REGIONS`, so a window of presents compares the switch on and off over the same span.

Lines to read, once per report:
- `[ORG] executions a frame: X over Y epochs (averages over 300 frames)`: compare `CS_ORG_REGIONS=1` against off.
- `[ORG] frame regions: N executions held M anchor runs (R each, J joins); L lost to a build, F failed; submitted before: <cause> <count>, ...;
  anchors in their region's epoch: ...`. A region execution counts once, however many runs joined it. Joins are runs minus executions.
  A held execution whose submission failed counts under `failed`, not as an execution.
- `[ORG] Epoch CPU <segment> (native): ...` per segment, `[ORG] Async epochs: ...` and `[ORG] Run memory: ...`.

## Phase 4 measurements (2026-11-13, RTX 5080, Dev-Fast, integrate dda594b00)

All runs are background runs (`CS_TEST_BACKGROUND=1`).

**Exclusive imports and home states off.**
- With exclusive imports (`CS_ORG_NATIVE_EXCLUSIVE_IMPORTS=1`), the start and Whiterun shots match the default within about 1%, in quick runs and in tours. Routing the native context's work through the ledger fixed the flashing.
- Home states off (`CS_ORG_NATIVE_HOME_STATES=0`) needs `CS_ORG_NATIVE_IN_STREAM=1`. Without in-stream epochs, the engine's loading-screen draws fail and the load never ends. With them, quick runs and tours pass.
- An intermittent load stall was seen twice: the first load never finished, the loading screen kept drawing, and nothing was logged. Once it was an exclusive-imports tour, once a home-states-off quick run without in-stream, both on builds before c7423914c. It did not recur in four exclusive quick runs or two later tours. A watcher that dumps the game 60 s into a stalled load is ready for the next one.
- The Dragonsreach tour shot is not repeatable on this machine. A control run with default settings differs from the others by 22-42%, because the sun patches through the windows move between runs. Judge interiors by eye, not by that number.

**Regions** (`CS_ORG_REGIONS=1`): 46 executions a frame over 24 epochs, against 51 over 25 with them off.
- Per frame, 20 region executions hold 25 anchor runs (5 joins).
- Splits per frame, by cause:
  - a client's second commit: 8;
  - an anchor with its own epoch: 6;
  - another epoch: 3;
  - another region's anchor: 2;
  - external commands: 1.
- The next gains are anchors whose clients commit twice (they could join like ImageSpace does), and hand-written clients that are not gated yet: Scaleform, DCLF's draw families, Bink, ImGui, SSS, IBL.

**Frame rate** (`native-vulkan+perf-quick`, vsync off): default 30.2 fps, regions 29.3, exclusive imports 28.1, home states off 30.9.
- The samples within one run spread from 23 to 37 fps, so these differences are noise.
- Dev-Fast (/Od) on this machine is CPU-bound, so the GPU savings that home states off should bring (resting layouts) cannot show. Measure them in a Release build, or on the RX 6600, where the 1.2 ms was found.

## Phase 4 item 5 lane: present through a graph tail (`org-native/p4-present`)

`CS_ORG_NATIVE_GRAPH_PRESENT` (native Vulkan only, off by default) presents the way BasicRenderer does:
- the back buffer is a graph-owned slot with one version per present slot;
- an `ExternalWork` tail copies the current version into the acquired swap chain image.

`=1` makes one version per present slot (3). `=single` makes one version, so there is no version change each frame. `0` or unset keeps
the hand-written copy.

**How it works** (`NativeDevice.cpp`, `NativeState::GraphBackBuffer`).
- **The versions.** `RenderGraphRuntime::Initialize` calls `Native::OnGraphReady` once the host is up, before any import. The native
  device then makes the versions as graph-owned textures (`MakeTexture`, so each is a persistent `PixelBuffer` with its own record):
  - they have the back buffer's size, format and bind flags;
  - they become one `org::VersionedTexture`, the first current and the others made ahead (`Make`), as BasicRenderer's
    `m_presentationColorVersions` are;
  - each version's image is registered as a made image of the slot (`RegisterMadeImage`, `SetMadeImageSlot`).
- **The rename.** The engine keeps one `ID3D11Texture2D` for the back buffer and its views. Its record gets `renamed`, the current
  version's record, and `Resolve` answers it for every handle of the back buffer. So:
  - `DescribeResource` names the current version's image;
  - `FrameAnchors::EngineTarget(kFRAMEBUFFER)`, `ShareImage` and `ImportTexture` find it with `MadeImageOf` and return the slot;
  - every pass that writes the back buffer (ImageSpace's last draw, upscaling, HDR output, Bink, Scaleform, ImGui, DCLF's blood
    splatter) declares the slot, and the preparation binds the current version;
  - the context's raw work (clears, capture readbacks, the present probe) runs on the current version's record, through `RouteRaw`
    when the graph moves images.
- **The tail.** On the render owner (`FrameAnchors::IsRenderOwner`), `Present` calls `PresentThroughLedger(version, ..., tail)`:
  - `ExternalWork::Submit` declares the version as a copy source (`ExternalWork::CopySource`). The client moves it there from the
    ledger's state, after the frame's last write. It flushes held regions first (`FlushRegions`).
  - The swap chain image is not the graph's. The tail records its barriers: Undefined to CopyDest, the copy, then CopyDest to Present.
  - The submission signals the present timeline, and `swapchain->Present` waits on that point. Acquire, present semaphores and frame
    pacing are as before.
- **The next version.** After the present, `NextGraphVersion` adopts the next present slot's version, renames the back buffer to it and
  calls `GraphHost::NoteNewVersions`. Tickets prepared for the old version are prepared again. The client keeps each version from its
  next writer until its tail's copy is done, so frame N+1 can draw while frame N's copy runs.
- **Home states.**
  - On (default): the client is closed and gives the version back in Common. The versions keep their host home.
  - Off (`CS_ORG_NATIVE_HOME_STATES=0`): the client is open and leaves the version in CopySource. The ledger carries that to the next
    frame that writes it.
- **Copy size.** The copy takes the smaller of the version's size and the swap chain's. They are equal unless a mode change made the
  swap chain again alone. Formats are equal by construction (`BackBufferFormat`/`SwapchainFormat`), HDR10's R10G10B10A2 included.

**Fallbacks** (each logged once).
- A present on a thread that does not own rendering (a loading screen or the logo without `CS_ORG_LOADING_THREAD`): it copies the
  current version on the old path and does not change versions. Versions are published on the render owner only.
- The client refuses the tail: the old path for that present, and the version stays current.
- D3D12 (`CS_ORG_NATIVE_BACKEND=d3d12`): `graph present ... not on D3D12`, and the old path.
- The versions cannot be made: `back buffer version N could not be made`, and the old path.
- Guest (DXVK) mode never reaches this code.

**Resize.** `ResizeBuffers` drops the slot with the old back buffer, makes new versions at the new size as a new slot, and asks for one
structural rebuild (`native back buffer resized: its graph present slot is made again`). Graphs built with the old slot declare it until
that build is attached.

**Log lines.**
- `[ORG native] graph present (CS_ORG_NATIVE_GRAPH_PRESENT): the back buffer is a graph-owned slot of 3 versions ...`, once at startup.
- `[ORG] frame anchors: cs.engine.target.0 is the image of 'Native::BackBuffer', which ORG made; it is that graph resource's slot`
  (and the same for Scaleform's, ImGui's and Bink's imports).
- `[ORG native] graph present: a present on thread ...` on a loading screen without the loading thread.

**What to check.**
1. `native-graph-present-quick` against `native-quick`: the shots match. The probe's `mean RGB` lines are close, with the same share of
   pixels lit.
2. A native tour with `--set org.native_graph_present=1`, against one without. Watch:
   - menus, the console and loading screens for flicker: a frame now starts from the contents of the version used three frames ago;
   - the frame rate: `NoteNewVersions` each frame prepares the tickets prepared ahead again.

   If either is worse, try `single`.
3. With `CS_ORG_NATIVE_HOME_STATES=0` and `CS_ORG_NATIVE_IN_STREAM=1`: the same shots. `native-validation` shows no new layout errors on
   the versions or the swap chain images.
4. With `CS_ORG_REGIONS=1`: the same shots, and no `external work ... while a region held` lines.
5. HDR output on: the HDR shots match, and the swap chain still reports HDR10.
6. `native-d3d12-quick` with the switch: the `not on D3D12` line, and the shots as before.
7. Look for `asks for a view ... lacks` naming `Native::BackBuffer`. Such an importer falls back to an import of one version's image.
   That import is cached by the D3D11 texture, so it would keep drawing into one version: two frames in three would show stale contents.

**Risks.**
- **Partial redraws.** With three versions, anything that draws over the previous frame without clearing sees an older frame. That is
  the contract of a flip-model swap chain's buffers, which DXVK hides behind its own single back buffer. `single` keeps the old
  contents.
- **`NoteNewVersions` each frame.** Recordings made ahead are tagged with the host's backing version, so one recorded ahead may no
  longer bind and is made again. DCLF already calls `NoteNewVersions` when it adopts versions, so this path exists, but now it runs
  every frame.
- **Raw handles kept across frames.** A client that keeps a raw image or view of the back buffer across frames (`Texture::Current()`
  kept, or a DCLF raw view baked into an epoch build) writes an old version. Those must declare the slot.
- **A present on another thread during a version change.** `MadeImageOf` drops the slot when the registered version is no longer
  current, so a describe between `Adopt` and the rename returns a plain resource for that call. Only work off the render owner can hit
  this.
- **Startup order.** The versions exist only once the host is up. Frames before that (the logo) copy the host-made back buffer as
  before.

## Graph present and correctness pass (2026-11-13, RTX 5080, integrate 9f5ebee3f)

**Graph present** (`CS_ORG_NATIVE_GRAPH_PRESENT`, values `0`/`1`/`single`; set it in cs-test as a string, `--set 'org.native_graph_present="1"'`).
- The back buffer is a graph-owned slot of 3 versions. Every importer resolves to it: `cs.engine.target.0` and the Scaleform target.
- Shots match the hand-copied present in a quick run (3.4% of pixels), and in a quick run with home states off, in-stream on and regions on (2.7%).
- A native tour with graph present and exclusive imports passes, and its start and Whiterun shots match.
- On D3D12 it logs "not on D3D12" and keeps the hand copy.

**Correctness.**
- Vulkan validation with home states off, in-stream on, regions and graph present logs the same 11 messages as the default settings, all the known unused SV_Target2 write. There are no layout errors.
- Two "hangs" in this pass were the render thread waiting on a synchronous compute shader compile, `DCLF::IndirectDraws::Impl::CreateMainBuffers` → `ComputeProgram::Load` → `ShaderCompiler::Compile`, slowed past the watchdog's 15 s:
  - a default-settings validation run;
  - one of two D3D12 quick runs, which hung at the compiling phase.

  Neither is caused by Phase 4. That compile belongs off the render thread, gated on readiness like DCLF's builds.
- No load stall with exclusive imports recurred: 4 quick runs, 2 tours, and the graph-present tour.

**Switch defaults are unchanged:** home states on, imports shared, regions off, graph present off. Turning them on by default is the next decision.

## Kept live recordings and coalesced submits (2026-11-14, `org-native/phases`)

**Where an execution's cost went** (perf-taa, native Vulkan, Perf-Fast, Tracy; per-anchor epochs, 52 executions a frame).
Tracy inflates the absolute numbers; the split is what matters.
- Render thread, 189 us an execution (9.8 ms a frame):
  - passes recorded at execution, 61 us (3.2 ms). 1.3 ms of it is real recording (about 99 passes), 0.95 ms is about 1,770
    gated passes that skip and record only their barriers;
  - queue submissions, 41 us (97 a frame, 2.1 ms): one per batch;
  - handoff lists 19 us, admission 16 us, the commit 10 us, upload lists about 13 us, the rest about 33 us.
- Host thread, 250 us a ticket (13 ms a frame): pass preparation 4.2 ms, the admission plan 5.1 ms, recording 1.8 ms, plus
  retirement 3.2 ms.

So preparation and recording scale with pass count on the host thread only. On the render thread most of the cost is fixed per
execution and per batch, plus the at-execution passes. The phase epochs (28 executions) made each execution larger: 167 queue
submissions a frame (late batches split their graphs into more batches), 2,140 skipped passes, and a host thread busy 23 ms a
frame. That is why they ran slower.

**What changed.**
- `CS_ORG_KEEP_LIVE` (native, default on; ORG `Desc::reuseLiveRecordings`): a live epoch's run binds the recording kept for its
  copy while every pass revision holds. The host thread prepares and records nothing for it. In the world all live runs bind a
  kept recording.
- A kept recording's at-execution passes now skip like a fresh recording's (`PreparedPass::SkipsAgain`). Before, a replay
  recorded every gated pass in full.
- `CS_ORG_COALESCE_SUBMITS` (native, default on; ORG `Desc::coalesceSubmissions`): an execution's consecutive batches on one
  queue are one queue submission (97 -> 57 a frame).
- `CS_ORG_PHASES` (default off): the phase epochs only on request. Per-anchor epochs are faster with the two options above.

**Results** (perf-taa, frames per second between the marks, no capture):

| Shape | Before | Keep only | Coalesce only | Both |
|---|---|---|---|---|
| Per-anchor epochs (52 executions) | 62.0, 66.1 | 69.6 | 72.6 | 73.0 |
| Phase epochs (28 executions) | 48.5 | - | - | 52.5 |

Tracy, per-anchor, both on: render thread epoch submission 9.8 -> 7.9 ms a frame, host thread busy 16.1 -> 4.1 ms.

**What is left on the render thread** (Tracy, 7.9 ms a frame): at-execution passes 2.8 ms (0.9 ms of it skipped gated passes),
queue submissions 1.2 ms, handoff 0.7, admission 0.7, commit 0.5, upload lists about 0.6, the rest 1.6 ms. Merging epochs does
not reduce this while gated passes record at execution and admission grows with the graph: a merged graph pays for every
pass that did not run. Gating on the GPU (indirect counts from latches, recorded once) would let gated passes leave the
at-execution class.

## Submission census and cheap fixes (2026-11-14, `org-native/phases`)

**Census.** With `CS_ORG_EPOCH_STATS=1` the statistics report adds ORG's submission census (`org::diagnostics::Census`,
`SubmissionCensus.h`): command lists a frame by kind, queue submissions by kind and why each is apart from the one before it,
open admissions by how they found their plan, handoff cache misses, and the sequencer's hold and waits.

**What it found** (perf-taa, native Vulkan, 52 executions a frame, before the fixes):
- Lists begun a frame: 156 upload lists (52 graphics, 104 queue-scoped), of which 152 recorded nothing; 43 handoff lists (25
  more reused); 24 inline. The 57 batch lists are kept recordings and are not begun again.
- Queue submissions: 54 batch submissions, 2 upload, 1 queue-scoped upload. No release or home-acquire submissions in this
  scene. 48 of the batch submissions follow an earlier ticket's on the same queue, and the one before already waited for
  everything they wait for. Only holding a ticket back could merge them (the submission thread's job).
- 22 admissions a frame missed their prediction because the entry states differed. Most alternate between frames.
- `vkQueueSubmit` is 20 us a call; the rest of `Queue::Submit`, lock included, about 2 us. The sequencer and
  `timelinesMutex` show no contention.

**What changed.**
- A ticket without pending uploads begins, records and closes no upload list (`IUploadService::HasPendingUploads`). The
  host's thread no longer begins them ahead. 6 upload lists a frame instead of 156.
- A prediction slot keeps its last three plans besides the current one. 16 of the 22 misses now find an earlier plan.
- The handoff cache keeps four plans' lists per frame slot. 13 handoff lists recorded a frame instead of 43.

| perf-taa, Tracy | Before (`66285f8ca`) | Upload lists | Plans and handoffs |
|---|---|---|---|
| Render thread `SubmitTicket` ms a frame | 6.13 | 5.85 | 5.30 |
| Lists begun a frame | 223 | 73 | 43 |
| Queue submissions a frame | 57 | 57 | 57 |

What is left on the render thread (5.3 ms): queue submissions 1.4 (58 calls), at-execution recording 1.4, commit inputs 0.5,
admission 0.5, `ORG.Persistent.Commit` 0.35, the revision check (`CheckTicket`) 0.3, handoff 0.23. The submission count and
the at-execution recording need the submission thread and segment lists.

## Submission thread (2026-11-14, `org-native/phases`)

`CS_ORG_SUBMIT_THREAD` (native mode; off with `CS_ORG_PHASES=1`; guest mode and external epochs keep submitting on the render
thread): ORG `Desc::submissionThread`. `SubmitEpoch` is split in two (`GraphHost::CommitTicket`, `FinishTicket`).

- **Render thread** (`CommitTicket`): the lease and services, the deferred releases, the caller's commit (`beforeSubmit`), the
  recording choice, the revision check and, for a stale ticket, its re-preparation. The check and the re-preparation stay here
  because each pass's revision and preparation read the caller's state as the commit left it. Then the commit's uploads are taken
  from the upload service (`RenderGraph::TakePendingUploads`) and the ticket is pushed onto the thread's queue.
- **Submission thread** (`FinishTicket`): the upload lists, the admission, the passes recorded at execution, the queue
  submissions and the completion message. The queue is FIFO and bounded (64 entries; a full one makes the render thread wait).
  `DrainSubmissions(why)` blocks until everything pushed so far is on its queue. A failure on the thread is sticky: the next
  `SubmitEpoch` throws it (the graph stops as for a failed submission) and `PollEpochFailures` reports it.
- **Merging tickets' queue submissions** is in the next section. A first attempt was reverted for a corruption whose cause
  was then found there.

What the thread changed elsewhere:
- The host's mailbox takes a post lock (two threads post).
- `SubmittedPoint` taken while tickets are queued has a pending part (`GpuPoint::Pending`): a marker in the queue that the thread
  fills in. It is not reached, and its waits wait, until the thread passes it. `GpuPoint::Resolve` gives the values, waiting first.
  `SealSubmitted` drains first, so `SealedPoint` never has one.
- Uploads: a commit's uploads are taken at its end (recorded lists, the upload pass's copies, the staged batches), and recorded
  with its ticket on the thread. Uploads queued later belong to later tickets.
- Releases: a queued ticket holds a `Runtime::AssignmentScope` until its values are published. Deferred deletions already waited for
  no open scope before sealing; descriptor releases and freed pinned arena regions now do too. Without this, a release on the
  render thread after a ticket was pushed would take a frontier that misses the ticket.
- The render owner drains where it flushes regions for its own queue work (external work, the immediate context, `EmitCommands`,
  a wait for the GPU), at the present, and before render ownership moves. Other threads' work needs no drain: it is admitted
  under the same sequencer, against the ledgers as they are.
- Passes recorded at execution now record after the render thread moved on. Those that read a slot the commit wrote (the sky and
  water draws' `client->run`, the deferred composite's execution, DCLF's CPU site runs) now read the value stored for their own run
  (`RenderGraph::RunSlots::Find`, `Direct::AdoptSiteRun`).

**Results** (perf-taa, native Vulkan, Perf-Fast, one run each; fps between the marks):

| | Thread off | Thread on |
|---|---|---|
| fps, no capture | 84.5 | 113.6 |
| fps, Tracy capture | 85.8 | 118.6 |
| Render thread graph time (`SubmitEpoch`), ms a frame | 5.33 | 0.93 (commit 0.84, enqueue 0.03) |
| Submission thread busy, ms a frame | - | 3.98 (submit 3.28: at-execution recording 1.45, `vkQueueSubmit` 1.19, admission 0.42, handoff 0.19) |
| Host thread busy, ms a frame | 2.87 | 4.06 |
| `vkQueueSubmit` calls a frame | 64 | 62 |
| Render thread blocked in drains, ms a frame | - | 0.75 (the present, once a frame) |

The queue's peak depth was 17 entries; backpressure never waited. With DLSS the Streamline evaluate drains once a frame too
(about 1 ms), and a few frames drain for the HDR UI texture's clear on the immediate context.

## Merged ticket submissions (2026-11-14, `org-native/phases`)

`CS_ORG_MERGE_TICKETS` (native mode, default on, with the submission thread and coalesced submits; ORG
`Desc::mergeTicketSubmissions`). The submission thread finishes the tickets queued together as one group. A ticket's last group
of batches is held, and the next ticket's first group joins its `vkQueueSubmit` when both are on one queue and the new group
waits for nothing the held one did not.
- Any other submission of the next ticket submits the held group first. So does recording a batch at execution: a pass recorded
  there may submit work of its own.
- The sequencer stays held across the group, so no other submitter comes between a held group and the ticket that joins it.
- Completion messages are posted once the group's last submission is on its queue.
- A wait on a value the held group signals becomes a full barrier in the joined submission.
- Only what is already queued is grouped: the thread never waits for more tickets.

**The corruption of the first attempt** (tiles of sky and fog colour over the tree and the sky) was a missing barrier in ORG's
admission, not in the merge.
- An execution's entry barrier was left out when the state an earlier execution left matched the one it needs: a render target
  written as a render target, a depth written as a depth. Nothing then ordered the second write after the first. Queue order does
  not order two render passes on one target. Separate `vkQueueSubmit` calls hid it on NVIDIA.
- The smallest failing join: one EngineFrameClear execution joined to the one before it. Each records every frame-group view's
  clear pass (a gated indirect draw with load and store), so their render passes on the same targets raced. A full barrier at
  that one join removed it (3 of 3 runs with DLSS clean). A barrier at the frame clear's other joins (after DepthDone, ImageSpace
  or the shadow clears) did not.
- The fix: entry barriers between identical states are kept when either side writes (ORG `fix(admission): order writes across
  executions`). Reads between identical states still need none.
- With the fix and merging on, 5 of 5 native-quick runs with DLSS were clean.
- Why it was hard to see: without the fix the corruption showed in 14 of 18 runs with DLSS and stayed for the whole run. About
  half the runs never register `cs.upscaling` before the shot, and those unantialiased frames also score as tiled on a grid
  detector. So every conclusion needs runs whose log has `cs.upscaling registered` before the shot, and several of them: the
  earlier bisect judged single runs. Synchronization validation saw nothing (it cannot see bindless accesses).

**Results** (perf-taa, native Vulkan, Perf-Fast, Tracy capture, one run each; both with the admission fix):

| | Merge off | Merge on |
|---|---|---|
| fps between the marks (capture) | 118.0 | 121.8 |
| `vkQueueSubmit` calls a frame, all threads | 62.2 | 43.6 |
| Tickets joined into the previous ticket's submission, a frame | 0 | 18.6 (50 tickets in 20.8 groups) |
| Submission thread busy, ms a frame (Tracy zones) | 3.79 | 3.80 |
| of it `vkQueueSubmit` (with held flushes) | 1.20 | 0.99 + 0.34 |
| of it recording at execution | 1.20 | 1.46 |
| Render thread graph time (`SubmitEpoch`), ms a frame | 0.96 | 0.91 |
| Render thread blocked in drains, ms a frame | 0.61 | 0.60 |

The submission thread spends what it saves in `vkQueueSubmit` on recording at execution and the flushes; the frame gains a little.

## The present as a queue entry (2026-11-14, `org-native/phases`)

`CS_ORG_QUEUE_PRESENT` (native mode with the submission thread, default on, `=0` off): the render thread no longer drains the
submission thread at the present.
- **Seal.** ORG `Desc::queuedSeals`: `SealSubmitted` queues the seal behind the tickets already queued. Until the thread passes it,
  `SealedPoint` has a pending part, so its waits wait for the thread first (it never waits for the render thread). `Seals()`
  counts the queued seal at once, so the render thread's later commits are behind it. The lease logic keeps counting the seals the
  thread passed. `PassedSealedPoint` is the last passed seal, for checks that must not wait (DCLF's `Sealed`, diagnostics).
- **Present.** `Native::QueuePresent` on the render owner takes what the render thread's state decides: the version the frame drew
  into, and for the graph present's tail the move to the next version. The copy, the swap chain's present and the frame-latency
  wait run on the submission thread, as a closure in queue order (`GraphHost::EnqueueOnSubmitThread`).
- **Pacing.** The render thread queues a present only once the one before it has finished. So it runs at most one frame ahead
  of the submission thread, and the swap chain's own wait paces that thread. The statistics report counts these waits.
- **What needs the present's result.** A resize, a colour-space change and the window closing drain the thread before they take
  the present lock. A failed present sets the device's `lost` or `resizeRequested` flag on the submission thread. The next present
  reads them there, as before. `WaitPresents` runs after those drains, so it sees every queued copy.
- **Other threads.** A loading screen's thread that does not own rendering presents synchronously after a drain, as before.
  Ownership moves drain first. The present probe (`CS_ORG_NATIVE_PRESENT_PROBE`) keeps the present on the render thread: it
  copies through the immediate context.
- **Not moved:** the Streamline evaluate (DLSS) still drains. It records through Streamline on the calling thread with that
  frame's constants, and moving it needs the constants and the evaluate call itself on the submission thread.

| perf-taa, native Vulkan, Perf-Fast, Tracy (measured) | Off | On |
|---|---|---|
| fps between the marks | 119.8 | 122.5 |
| Render thread blocked at the present (drain, then pacing), ms a frame | 0.60 | 0.00 |
| Render thread `CS.Present`, ms a frame | 1.24 | 0.40 |
| Render thread busy (zone union), ms a frame | 4.25 | 3.44 |
| Submission thread: the queued present, ms a frame | - | 0.28 |

native-quick with DLSS registered: 3 runs on and 1 off, all clean by eye. `CS_ORG_LOADING_THREAD=1` passes.

## Gated copies and clears kept by their gate answers (2026-11-15, `org-native/phases`)

`CS_ORG_KEEP_GATED` (native mode, default on, `=0` off; ORG `Desc::keepGatedInline`).
- **What.** A gated copy or clear is recorded at execution only so that a closed gate can skip it. Its commands do not depend on the
  run otherwise. Such passes now say so (`static constexpr bool kRecordKeyedBySkip = true`): AnchorPasses' copies, Dynamic
  Cubemaps' copies, Grass Collision's copy, the skeleton's clears, Terrain Blending's depth clear, the deferred G-buffer clear and
  the UAV clears (Screen Space Shadows, Subsurface Scattering, IBL).
- **Mechanism.** A kept recording's inline late batch made only of such passes keeps its list. At each submission the submission
  thread asks each pass's skip answer for the run (`SkipsAgain`, after the commit). When the answers equal those the list was
  recorded with, the list is submitted again as it is. Otherwise it is recorded again, once, for the new answers.
- **Why not recorded ahead with the early batches.** The early batches are recorded on the host's thread before the run's commit,
  when the gates are not known yet. Putting the answers into the passes' revisions would make the render thread's revision check
  fail on every pattern change, and that path waits for the host's thread to prepare the ticket again. Keeping the late list by
  answers records on the submission thread only when the answers change, and never waits. The answers are also what a segment
  list's signature needs.
- **Not covered.** A late batch with a live pass in it (the sky and water draws, Scaleform, ImGui, the deferred composite, DCLF's
  direct sites, the IBL sky SH dispatch, the sun occlusion) is recorded at every submission as before. The census line
  `inline lists a frame` names the first live pass of each recorded batch.

| perf-taa, native Vulkan, Perf-Fast, Tracy (measured) | Off | On |
|---|---|---|
| fps between the marks | 121.2 | 123.8 |
| Inline lists recorded a frame (census) | 25.0 | 12.5 (9.2 kept, 0 re-recorded for changed answers) |
| Passes recorded at execution a frame (`RecordOwned.Pass` on the submission thread) | 46.9 | 35.8 |
| Submission thread `RecordInline`, ms a frame | 1.17 | 1.00 |
| Submission thread busy (zone union), ms a frame | 3.69 | 3.54 |
| Inline batches submitted a frame | 23.9 | 23.9 (13.9 of them recorded) |

native-quick with DLSS registered: 3 runs on and 1 off, all clean by eye.

## Segment census: what one list per segment would meet (2026-11-15, `org-native/phases`)

Step 3 of the plan (one command list per frame segment) stopped at its first part, as a measurement. With `CS_ORG_EPOCH_STATS=1`
the host groups each frame's admitted tickets by segment on the submission thread (ORG `GraphHost::SetSubmissionSegment`,
`SetSegmentCensus`, `TakeSegmentStats`) and compares each segment's signature with the segment's last four: the ordered epochs,
each ticket's recordings and its admission plans. CS names the segment at each `SubmitEpoch`: the ten segments of the plan, cut
at DCLF's epochs (`FrameSegmentOf` in RenderGraphRuntime.cpp). No list is built and nothing is held.

perf-taa, native Vulkan, one window (measured, per frame):

| Segment | Runs | Executions | Signature not among its last four | Admissions without a predicted plan |
|---|---|---|---|---|
| pre-shadow | 1 | 8.2 | 0 | 0 |
| DCLF shadows | 1 | 0.9 | 0 | 0 |
| pre-depth | 1 | 1.8 | 0.92 (recordings, plans) | 0.92 |
| DCLF depth | 1 | 0.9 | 0 | 0 |
| pre-main | 1 | 8.2 | 0.92 (recordings, plans) | 2.75 |
| DCLF main | 1 | 0.9 | 0 | 0 |
| deferred and water | 1 | 14.0 | 1.0 (recordings, plans) | 4.0 |
| DCLF forward and first person | 2 | 2.0 | 1.0 (order) | 0 |
| post | 2 | 11.0 | 1.0 (order, recordings, plans) | 1.0 |
| interface | 1 | 2.0 | 1.0 (recordings) | 0 |

What it shows:
- **Four segments are already stable** (pre-shadow and DCLF's shadow, depth and main epochs). A kept list per signature would be
  recorded once for them.
- **Copies are not pinned by occurrence.** The anchors that run many times a frame take whichever kept copy is idle first
  (`TakeLiveCopy`): image space 12.75 times a frame, the frame clear 6.6, the shadow clear 3.8, the engine draws 2.8, the cube
  clear, the reflection sky and the depth copy about 2. So the set of recordings in a segment differs from frame to frame, though
  each copy keeps its recording. A segment list needs each occurrence of an epoch in a segment to take the same copy every frame
  of the same frame slot.
- **About 8.7 admissions a frame miss their predicted plan** (pre-main 2.75, deferred and water 4, post 1, pre-depth 0.9). Each
  would change a segment's barriers. They must reach stable plans, or stay out of kept segments.
- **The cut table is not quite the frame's order.** DCLF's forward and first-person epochs are apart (post work runs between
  them), and post runs as two pieces around them. The table needs a cut there.
- **Holding a segment until its cut** is what one list per segment needs: the list can only be chosen once the segment's last
  ticket is admitted. The current group merge holds the device's sequencer across a group. Held across a whole segment it would
  block other threads' submissions for the render thread's time, and a wait on the render thread for such a submission would
  deadlock. Held submissions were dropped earlier for device loss. So a segment list needs a hold without the sequencer, or
  submission ahead with a GPU wait on a CPU timeline. Neither was attempted here.

## The declared frame, stage 1: the frame driver and the pre-depth segment (2026-11-15, `org-native/phases`)

Stage 1 of the declared-frame plan: CS decides what a frame contains at its start, and the EarlyPrepass clients run as one
segment recorded per description. `CS_ORG_SEGMENTS` (native mode, default on; `=0` off; in this stage `=2` meant open executions
instead of closed; since stage 3 the default is open, `=3` closes them, and `=2` no longer differs from the default). Guest mode keeps the per-anchor path.

**The frame driver** (`src/RenderGraph/Frame/FrameDriver.*`, `FrameDescription.*`).
- At each `Renderer::Begin` (SE 75460, AE 77245; the detour in `Engine/LoadingScreen` is installed in every mode now) it builds
  the frame's description: a plain value that hashes and diffs field by field.
  - **Kind**, from state at Begin, never from the census flags (written after the present, one frame stale):
    - a Begin inside `BSMoviePlayer::Tick` (SE 87890, AE 90259) is a **Bink movie**'s frame. The Bink hooks mark the tick; without
      them the driver detours the tick itself.
    - a Begin on another thread than the main one is a **loading screen**'s (the loop of `SetLoadingScreenActive`, SE 34552,
      AE 35407, which the driver also follows).
    - on the main thread, the engine's own test decides: Main's frame (SE 35556, AE 36555) calls `MenuManager` SE 79938 / AE
      82075 right after Begin. It scans the menu stack under its lock for flag 0x20 (freeze background). Without such a menu
      the world is drawn when the player's `Get3D2` is set (**world**), else not (**main menu**: the main menu, or any frame
      before the player has 3D). With one, the world is drawn once (SE 35558 / AE 36558, which also needs the player's cell)
      and composited after (**frozen world**, `capture` set on the frame that draws it). Found with Ghidra: `Main::Draw`
      (SE 0x1405b1710) does not run at the main menu.
  - **Frame number, slot** (frame modulo `kGameFramesInFlight`) **and parity**.
  - **Segment fields** (`SegmentFields`), filled by `FrameAnchors::BeginFrame`: declared (world, or a frozen world's capture
    frame), the segment graph's build, the segment's version generation (below, "Versions"), and a bit per client: on or
    off this frame, from `ClientDesc::declared` (settings and scene state the client holds; null means on).
- Each change of kind is logged (`[ORG] frame kind: kind world -> loading screen (frame N)`). With `CS_ORG_EPOCH_STATS=1` the
  report adds `[ORG] declared frame: frames by kind: ...; pre-depth: declared, taps, recordings, waits; tap errors`.

**The pre-depth segment** (`FrameAnchors.cpp`: `kDeclaredInfo`, `RunDeclared`, `BeginFrame`; `Segment::DeclaredPreDepth`).
- One epoch, revision-driven and closed. Its graph holds the EarlyPrepass clients' passes (the phase code's composition, by
  member anchors), all recorded ahead: the gated copy and clear that were recorded at execution only to skip run with their
  client's bit instead. EarlyPrepass has no epoch of its own then.
- **Recording.** A description's recording is asked for at Begin, for the frame slot's copy alone (`RequestEpochRecording`'s
  copy), so the host's thread records it while the engine draws the shadow maps, and it waits for no GPU work. The last six
  descriptions per slot are kept. The revision is the segment fields' hash, and the host data (`SegmentHostData`) turns each
  off client's passes into barriers only (`RenderGraphPass::SetSkipWhen`). A build or a new version between Begin and the tap
  is read again at the tap and recorded for.
- **Slots.** The segment's runs take the published copy of their frame slot (`GraphHost::SetEpochCopy`); the copy's run
  (`EpochCopyRun`) is the slot's, so its latch regions, gate entries and run slots are the slot's. They are zeroed before the
  commits, as a new lease's are.
- **Taps.** The EarlyPrepass hook is the segment's tap and its end. The clients' commits run there for the clients on in the
  description, and the segment's one ticket is submitted. Tap errors (`[ORG] frame tap error (<kind>)`, counted in the census):
  a tap in a frame whose description has no such segment, a second tap in a frame, an active client the description turned
  off, a declared segment whose tap did not fire by the next Begin, a run that is not the slot's copy, and no recording.
  Nothing else happens on an error: the tap draws nothing.
- **Private alias pools.** The segment's transients are kept out of the shared alias arena (`SetPrivateAliasEpochs`). In the
  shared arena another epoch's transients took their memory between frames, every admission found alias activations, and the
  segment planned against the ledgers every frame: 63 us a submission closed and 27 open, against 12 with pools of its own.

**What ORG got** (`extern/OpenRenderGraph`, documented in persistent-epochs.md, "Segments"): `SetEpochCopy`, `EpochCopyRun`,
one-copy recordings, `SetClosedEpochs`, `SetPrivateAliasEpochs`, `EpochRecordingVersionsCurrent`, `RenderGraphPass::SetSkipWhen` with
`PreparedPass::SkippedAhead`, and per-epoch barrier counts in the segment census (`SegmentStats::barriers`), printed as
`[ORG] Barriers a frame, by epoch`. Test: `PersistentVulkanHostTests segment`. Reused as they were: revision-driven epochs,
`EpochRecording::copies`, candidates and `UseEpochRecording`, closed admissions, pinned latch regions of published copies.

**Measurements** (perf-taa, native Vulkan, Perf-Fast, Tracy capture, one run each; measured unless marked):

| perf-taa, native Vulkan, Perf-Fast, Tracy (measured) | Off (`CS_ORG_SEGMENTS=0`) | On (default, closed) |
|---|---|---|
| fps between the marks | 123.3 | 123.4 |
| Tickets a frame (census) | 51.8 | 52.0 |
| `vkQueueSubmit` calls a frame (all threads) | 43.4 | 43.9 |
| Batch lists a frame (census) | 56.9 | 55.9 |
| Inline lists recorded a frame (`RecordInline`) | 23.8 | 22.0 |
| Passes recorded at execution a frame (`RecordOwned.Pass`) | 41.5 | 38.9 |
| Render thread graph time (`SubmitEpoch`), ms a frame | 0.867 | 0.868 |
| Render thread busy (zone union), ms a frame | 3.19 | 3.19 |
| Submission thread busy, ms a frame | 3.70 | 3.60 |
| Host thread busy, ms a frame | 3.28 | 3.15 |

The frame-level differences are inside one sample's noise (an earlier pair on another build read 124.0 against 126.3 fps).

The per-segment cost against the per-anchor cost, for the same clients (EarlyPrepass's epoch with segments off, the segment's
with them on):

| Per run (measured) | EarlyPrepass's epoch (off) | Segment, closed, pools of its own (on) | Segment, open, shared arena (earlier build) | Segment, closed, shared arena (earlier build) |
|---|---|---|---|---|
| Render thread (`ExecuteEpoch`), us | 17.3 | 23.5 | 21.9 | 22.7 |
| Submission thread (`FinishTicket`), us | 69.9 | 68.2 | 96.9 | 121.0 |
| of it the admission | 13.1 | 13.2 | 27.5 | 62.8 |
| of it recording at execution | 2.1 | 0 | 0 | 0 |
| of it `vkQueueSubmit` | 29.8 | 28.7 | 25.3 | 31.4 |
| Entry barriers | 42 | 41 | 42 | 41 |
| Body barriers | 43 | 279, and 1 full barrier | 43 | 279, and 1 full barrier |

The model holds on this segment: one ticket for the same clients costs what the anchor's did on the submission thread and 6 us
more on the render thread (the recording lookups, the version check and zeroing the slot's latch regions), with nothing recorded
at execution. The earlier attempt at larger graphs lost on admission; here the admission takes its predicted plan once the
segment's transients have pools of their own.

- **Barrier cost of closing.** The segment's body holds 279 barriers and one full barrier a run closed, against 43 open (the
  per-anchor epoch's body also holds 43). Closing returns every resource it touches to its home state after its last use,
  per subresource range. Its entry barriers do not drop yet (41 against 42): the epochs before it are still open. The GPU cost
  of the extra barriers was not isolated; the frame rate does not show it at one sample.
- **Re-records.** In the steady perf-taa window: 0 recordings and 0 waits over 901 frames, taps equal to declared frames, 0 tap
  errors. A new description is recorded once per slot, all four asked for at the frame's start; only that frame's tap can wait.
  The validation tour (loads, a save and its load, an interior) recorded 16 copies: the first description and two startup graph
  builds, and Dragonsreach turning Volumetric Shadows off (no directional shadows). Two taps waited, 8.4 ms in all, both at
  startup.
- **Versions.** A recording binds the versions it was recorded with. The first version of this stage put the host-wide count of
  `NoteNewVersions` in the description: Drawcall Limit Fix adopts grown buffers mid-frame, so the segment was recorded again 88
  times in the validation tour, mostly at its tap, waiting up to 54 ms a window with the validation layer on. The description now
  carries the segment's own version generation, moved only when a kept recording binds a version that is no longer current
  (`GraphHost::EpochRecordingVersionsCurrent`, checked at Begin and at the tap).

**Checks** (the final build, segments on by default):
- native-quick, Vulkan: 3 runs with the upscaler registered before the shot, clean by eye against a run with segments off; 0 tap
  errors.
- native-d3d12-quick: matches its segments-off run by eye.
- native-river, the loading-thread run (`CS_ORG_LOADING_THREAD=1`) and guest-quick (segments stay off in guest; the driver still
  describes the frames): passed, 0 tap errors.
- native-validation: the known 20x `VUID-vkCmdDraw-None-09600` and the BackBuffer layout warning, nothing else new.
- Frame kinds against what ran: the startup logo (Bink movie), the main menu, the loading screens of the autoload and of the save's
  load, and the world, matching the taps (the pre-depth tap fired in every world frame and in no other). A load
  within the world (coc Dragonsreach) kept rendering the world on the main thread with the loading menu open: the driver calls
  those frames world, and the taps agree; the census, which reads the menu flags, called them loading-screen frames.
- ORG: the suite passes but the three known lease hangs and `HeadlessBenchmark`'s Debug timeout; `PersistentVulkanHostTests
  segment` is new.

**What stage 2 should know.**
- The segment's exit barriers are many and their resting state is the resource's home, not what the next segment needs. A
  declared resting state per resource (the state the next segment enters it in) would make closing nearly free once all
  segments are closed.
- A tap before the segment's end point (stage 3's segments with several anchors) must write the slot's regions only once the
  slot's copy is free: take the segment's ticket at the first tap (`HoldEpochTicket` works for live epochs only today).
- The world and the frozen world's capture frame declare the pre-depth segment. No tour reached a frozen-world frame (the
  inventory and map in the validation tour did not freeze the background), so the capture frame's taps are unchecked.
- Copies and clears that were recorded at execution only to skip now run on every frame their client is on. A later stage that
  moves such a pass needs a description bit for it.

## The declared frame, stage 2: clears and copies (2026-11-15, `org-native/phases`)

Stage 2 of the declared-frame plan: the engine's clears and copies stop being tickets of their own in a world frame, and the
pre-depth segment closes without its exit barriers. Native mode with segments on. Switches: `CS_ORG_SEGMENT_CLEARS` (default on;
`=0` keeps every clear and copy on its own anchor's ticket) and `CS_ORG_SEGMENT_REST` (default on; `=0` closes segments to home as
in stage 1). Guest mode is unchanged: its skeleton keeps the per-anchor path.

**Resting states** (ORG `GraphHost::SetRestAtLastUseEpochs`; persistent-epochs.md, "Segments"). A closed segment now leaves each
resource in the state its last use left it, instead of going home. The resting state is a function of the segment's graph alone,
so the next execution finds the same states whenever the same description ran, and the segment has no exit barriers.

| Pre-depth segment, per run (census, measured) | Stage 1 (closed, home) | Stage 2 (closed, resting at last use) |
|---|---|---|
| Body barriers | 279, and 1 full barrier | 43, and 1 full barrier |
| Entry barriers | 41 | 42 |

The entry barriers stay: the epochs before the segment are still open, so what it enters from depends on them. The full barrier at
a closed execution's entry stays too. A per-resource resting state chosen by the next segment's first use (rather than the last use
here) is the next step once neighbouring segments are closed; with only one closed segment it would only move barriers from the
next epoch's entry into this segment's body.

**Early taps.** A tap that fires before its segment's end point writes into the frame's tap record on the CPU (the clear or copy it
saw, its colour and its gate entry), not into the slot's latch regions. The end point's commit writes the record into the run that
submits it (`RunGate::Close/OpenDraw/OpenDispatch(run, ...)`, the explicit-run forms added here, with `GraphHost::CurrentRun()` in
the commit). This is the defined answer for "the slot's copy may still be in flight": the slot's regions are touched only inside its
ticket's commit, after the host's thread took the copy (it waits there, on its own thread, only when the copy's last run is not
done; four frames back, that is not the case in a steady scene). The render thread never waits for it. A tap's record that no host
commit takes by the next `Renderer::Begin` is a tap error (`host missing`) and is dropped; a record whose host sees another epoch
submitted first is a tap error (`host skipped`).

**Private alias pools.** The pre-depth segment's pool is 1.33 MB (one transient; 1920x1080 output, 1280x720 render, perf-taa
scene). The shared arena is 53.6 MB, sized by the DeferredPasses epoch (53.6 MB of 67.9 MB independent), with VolumetricLighting
(11.3 MB) and nothing else aliased. If every segment had pools of its own, the frame would hold the sum instead of the largest:
53.6 + 11.3 + 1.3 = 66.2 MB, 12.6 MB more at this size, about 50 MB more at 4K. Proposed instead of private pools (not built): one
placement for the whole frame's segments in their fixed order, so each frame's alias activations are the same, and an admission
that treats a planned activation (entry from Undefined at the same placement every frame) as part of its predicted plan instead of
planning against the ledgers.

**Clears and copies.** Ghidra first (SE 1.5.97):
- The renderer's clear colour is `RendererData::clearColor` (renderer +0x2778). `Renderer::SetClearColors` (SE 0x140d6a6d0) pushes
  it and `SetClearColorFromArray` (SE 0x140d6a740) pops it. `Main::Draw` (SE 0x1405b1710) sets it once a frame to
  (sky colour x scale, alpha 1): `DAT_1431d0f90..98`, written every frame by the sky's update (SE 0x1403b1670, from Sky +0x138..0x140
  times `DAT_1415ab290`). Shadow maps push (1,1,1,1); the shadow mask pushes its own. So the kMAIN and G-buffer clear colour changes
  per frame with the sky: latched data, not a description field.
- The frame sequence census (`CS_ORG_FRAME_SEQUENCE`) now names each skeleton flush's views (`skeleton <group> clears: ...`). A world
  frame clears: four shadow slices before DCLF's shadow view; a reflection face and the faces' depth before each face sky; the main
  depth (flags 3) at Main::RenderDepth's start; kSHADOW_MASK before the shadow mask draws; kNORMAL_TAAMASK_SSRMASK and kMAIN in
  Main::Draw (then only light culling before the Prepass anchor); the main stencil (flags 2) before the water mask; kWATER_2 after
  the water surface, before the next ImageSpace batch; kTEMPORAL_AA_MASK and kMAIN_ONLY_ALPHA just before DCLF's forward epoch. The
  copies: kMAIN depth into kPOST_ZPREPASS_COPY at RenderDepth's end and in Deferred::DeferredPasses; kMAIN into its copy before the
  water surface.
- kWATER_1/2 and the TAA mask are cleared through the native immediate context, not a patched site; their first writers are the
  ImageSpace chain's water blend and DCLF's forward draws.
- The kMAIN clear is not redundant in general: the sky draws over it, but interiors have no sky and the clear colour shows where
  nothing draws.

| Clear or copy (engine call) | Before: native, every frame | Now: a world frame | Colour |
|---|---|---|---|
| Shadow slices, depth targets 2, 3, 4 (RenderShadowmap, about 4 a frame) | an EngineShadowClear ticket each | a gated depth-clear pass per slice view at the start of DCLF's shadow view graph | depth 1 |
| Precipitation occlusion map, depth target 10 (Precipitation::RenderOcclusion, rare) | an EngineShadowClear ticket | unchanged: a ticket (below) | depth 1 |
| Reflection face and the faces' depth (RenderFaces, 2 a frame) | an EngineCubeClear ticket each | first passes of the SkyReflections anchor (the face sky), its next run | renderer clear colour, latched by the tap |
| Main depth, flags 3 (RenderDepth's start) | an EngineFrameClear ticket | at the start of DCLF's Z-prepass graph | depth 1, stencil 0 |
| kSHADOW_MASK (RenderShadowMasks) | a ticket | first pass at EngineDraws (the shadow mask draws) | latched: (1,0,0,0) exterior, 0 interior |
| kNORMAL_TAAMASK_SSRMASK(_SWAP) and kMAIN (Main::Draw's ClearColor) | 2 tickets | first passes at the Prepass anchor (light culling, which runs between, does not touch them) | latched: (0.5,0.5,0,0); the sky colour |
| G-buffer 2-7 (Deferred's SRTM_CLEAR) | a pass in Prepass (ORGDeferredClears) | unchanged | renderer clear colour |
| Main stencil, flags 2 (FinishAccumulating) | a ticket | first pass at EngineDraws (the water mask) | 0 |
| kWATER_1/2 (native context) | a ticket | first pass at ImageSpace (the chain's water blend) | latched |
| kTEMPORAL_AA_MASK and kMAIN_ONLY_ALPHA (native context) | a ticket | unchanged: a ticket (below) | latched |
| Depth copy in Deferred::DeferredPasses | an EngineDepthCopy ticket | a gated copy, first at DeferredPasses (run just after it) | - |
| kMAIN into its copy (native context CopyResource) | an EngineCopy ticket | unchanged: a ticket (below) | - |
| Depth copy at RenderDepth's end | an EngineDepthCopy ticket | unchanged: a ticket (below) | - |

Why gated clear draws in the host and not load operations: the set of slices, faces and targets cleared, and their colours, change
from frame to frame (shadow lights come and go, the clear colour follows the sky). As load operations they would be baked into the
host's recording, which would then be recorded again whenever the set changed (DCLF's epochs are revision-driven). A gated
fullscreen draw per view is recorded once and does work only when the frame's tap opened it.

DCLF's epochs do not run in every world frame, so a DCLF host is used only when they will:
- `Description::dclfDraws`, read at Begin: DCLF's first claims are installed (it never goes back). Before that the engine draws the
  world itself, and every clear keeps its own ticket.
- DCLF's per-frame decisions, made before the frame's first clear and read at each tap: no load hold is on (`DCLF::EpochsRun`; a
  re-entry hold after a graph rebuild starts inside a frame, at the shadow coverage decision) and, for the shadow slices, DCLF draws
  this frame's shadow views (`DCLF::DrawsShadowViews`, decided at the start of CalculateAndDrawShadowCasterLights). This is the
  plan's shadow decision event: it completes the frame's description before the slices are cleared.
- The main depth's clear goes to the Z-prepass only inside Main::RenderDepth (`DCLF::InDepthPass`). The engine clears the main depth
  with the same flags elsewhere too: the inventory's 3D item and Dragonsreach both did it once more each frame (the validation tour
  showed them as `host skipped` before this check).
- A loading screen that starts inside a world frame ends that frame's world work (`Driver::LoadStartedThisFrame`): work still
  waiting for its host then is abandoned with it (counted as "abandoned at a load's start", not a tap error).

**What stays a ticket.** The TAA mask and kMAIN_ONLY_ALPHA clears (about one EngineFrameClear ticket a frame): cleared before DCLF's
forward epoch every frame, but the TAA mask is also cleared when TAA is turned on, inside the ImageSpace chain, so no one host follows
both call sites (the upscalers run showed it as `host skipped`). The kMAIN copy (about one a frame outdoors): outdoors the water
surface's draws follow it, in Dragonsreach the ImageSpace chain does. The precipitation occlusion map's clear: Precipitation renders
it at no fixed place before the shadow view (seen once in the validation tour). The depth copy at RenderDepth's end (about one a frame): its readers come after the first-person depth when
the view is first person, so its host would be the last of two DCLF epochs, and an ungated copy cannot be placed after DCLF's own
passes there reliably (the hosted passes go first; extension order is not stable). Every clear and copy outside a world frame (the
loading screen, the main menu, a Bink movie, a frozen world's composite frames) keeps its anchor's ticket until stage 5 gives those
frame kinds their segments.

**Measurements** (perf-taa, native Vulkan, Perf-Fast, Tracy capture, one run each; off = `CS_ORG_SEGMENT_CLEARS=0
CS_ORG_SEGMENT_REST=0`, which is stage 1; measured unless marked):

| perf-taa | Off (stage 1) | On, every host (first build) | On (final) |
|---|---|---|---|
| fps between the marks | 123.9 | 125.6 | 124.8 |
| Tickets a frame (census) | 51.95 | 36.91 | 40.31 |
| `vkQueueSubmit` calls a frame (all threads) | 44.0 | 36.5 | 37.4 |
| Batch lists a frame (census) | 55.9 | 47.9 | 46.3 |
| Handoff lists recorded a frame | 14.1 | 9.3 | 9.2 |
| Render thread graph time (`SubmitEpoch`), ms a frame | 0.881 | 0.867 | 0.950 |
| Render thread busy, ms a frame | 3.32 | 3.26 | 3.37 |
| Submission thread busy, ms a frame | 3.44 | 3.53 | 4.10 |
| of it `FinishTicket`, ms a frame | 3.21 | 3.26 | 3.23 |
| Host thread busy, ms a frame | 3.03 | 2.78 | 3.03 |
| Pre-depth segment body barriers a run | 279 + 1 full | 43 + 1 full | 43 + 1 full |
| Tap errors | 0 | 0 | 0 |
| Segment recordings in the steady window | 0 | 0 | 0 |

The first build hosted the TAA mask, kMAIN_ONLY_ALPHA, the kMAIN copy and the precipitation occlusion map too; the checks showed
those cannot follow one host (below), and the final build keeps them on tickets: about 3.4 more tickets a frame than the first
build, 11.6 fewer than stage 1. The final run's submission-thread busy time rose by 0.66 ms while its `FinishTicket` time did not
move; the difference is outside the tickets and was not explained in one sample.

The submission thread did not get cheaper. The removed tickets cost it about 0.47 ms a frame (per epoch, from `FinishTicket`); their
hosts grew by about 0.41 ms (admission of the extra resources in every run of the host, ImageSpace's 13 runs above all, and the
hosted passes recorded at execution in the live anchors), and the rest is inside one sample's noise (DCLF's forward recording moved
by 0.1 ms between the runs with nothing hosted that records there). The ticket and submission counts fall as expected (15 tickets,
7.5 submissions); the frame-rate difference is inside one sample's noise.

**Checks** (the final build; earlier builds where marked):
- native-validation: the known 20x `VUID-vkCmdDraw-None-09600` and the BackBuffer layout warning, nothing else; 0 tap errors over
  the tour (loads, the inventory, the map, a save and its load, Dragonsreach). Shots by eye against stage 1's: the same; Dragonsreach's
  brightness varies between runs in stage 1 too (86 to 99 mean over ten runs).
- native-river: 0 tap errors, shots match stage 1's by eye (water, reflections). native-upscalers: 0 tap errors, DLSS, FSR, TAA and
  XeSS shots match by eye. native-d3d12-quick, the loading-thread run (`CS_ORG_LOADING_THREAD=1`) and guest-quick: passed, 0 tap
  errors (guest has no hosting).
- native-quick: none of the final build's ten 400-frame runs registered the upscaler before the shot (`cs.upscaling registered`
  never appears in them), so they prove nothing for the image beyond 0 tap errors. Two runs of the first hosting build did register
  it (h2, c2q1) and were clean by eye; that build hosted more than the final one. No "clean" claim on three registered runs of the
  final build.
- The census found what each guard is for: DCLF's epochs not running before its first claims and in load holds, the main depth
  cleared by the inventory's 3D item and in Dragonsreach outside RenderDepth, the TAA mask cleared at a TAA switch, the kMAIN copy
  indoors, and the precipitation occlusion map, each at first as `host skipped`/`host missing`.
- ORG: the suite passes but the three known lease hangs and `HeadlessBenchmark`'s Debug timeout (Nsight layer disabled); the new
  `PersistentGraphTests` case passes.

**What stage 3 should know.**
- **Hosts are anchors that stage 3 moves.** The skeleton registers clients first at SkyReflections, Prepass, EngineDraws,
  ImageSpace and DeferredPasses (`<group>@<host>` ids). When those anchors become members of S0, S2 and S3 their hosted clients go
  with them: they are gated draws (`gpuGated`), so they work recorded ahead. The hosted depth copy (at DeferredPasses) is a gated
  copy, which has no indirect form: in a recorded-ahead segment it would run every frame. It happens in every world frame, so that
  is correct, but it needs a description bit if a frame can lack it.
- **Still tickets in a world frame:** the TAA mask and kMAIN_ONLY_ALPHA clears, the kMAIN copy, the depth copy at RenderDepth's end
  and the rare precipitation occlusion clear. Each needs its segment (S3, S4) or a description field to pick its place.
- **The copy at RenderDepth's end** is the one clear or copy left as a ticket in a world frame. Its place is S2's start, after the
  last depth writer (the first-person depth epoch when the view is first person): a description field for the first-person view
  decides which.
- **`RenderGraphRuntime::SetEpochHooks`** is where another module writes its run data into a DCLF epoch's commit, and where the
  order guard (`host skipped`) looks at every submission. `AllowedBetween` lists the epochs that may run between a tap and its host
  (light culling before Prepass); a new segment that moves an epoch between a tap and its host shows as a `host skipped` error.
- **Hosting costs admission in every run of the host.** ImageSpace (13 runs) pays for the kWATER clear's resources in each. Once
  the post chain is declared (stage 4) the clear belongs to the water blend's instance alone.
- **The description has `dclfDraws`.** A segment whose clients DCLF's epochs replace should read it at Begin as the hosts do.
- **Resting at last use** is on for every closed segment. Once two neighbouring segments are closed, a resting state chosen by the
  next segment's first use is the next step for the entry barriers.
- **Private pools** cost 1.33 MB for S1; the sum across segments would be about 12.6 MB over the shared arena at 1080p output. The
  proposal above (one frame-wide placement, activations in the predicted plan) is the alternative to weigh before every segment
  gets pools.

## The declared frame, stage 3: the pre-main and deferred segments (2026-11-15, `org-native/phases`)

Stage 3 of the declared-frame plan: two more segments with several taps each, and the ORG fixes that keep a segment's ticket as cheap
as the tickets it replaces. Native mode with segments on. Switches: `CS_ORG_SEGMENT_PREMAIN` and `CS_ORG_SEGMENT_DEFERRED` (default
on; `=0` gives the anchors back their own epochs), `CS_ORG_SEGMENT_POOLS` (default on: segments' transients in pools of their own;
`=0` leaves them in the shared arena), `CS_ORG_SEGMENTS=3` (segments closed; the default is now open, below). Guest mode is unchanged.
The frame-start segment (S0) and the latched instances for live passes were not built (below).

**Outcome.** perf-taa: 38.7 to 33.9 tickets and 37.6 to 34.3 queue submissions a frame against the same build with the two segments
off. The two segments cost the submission thread 195 us a frame against 362 us for the tickets they replace, render-thread time 21
against 75 us, and admission 16 against 34 us (measured, one sample each). Cost stayed flat as the segments grew once two ORG causes
were fixed: alias activations planned against the ledger at every execution, and per-cell state commits.

**Two checks first.**
- *The upscaler stopped registering before the shot.* Not a regression. Upscaling stops while another process has the foreground
  (`Upscaling::IsWindowUnusable`; `CS_UPSCALE_UNFOCUSED=1` lifts it). Stage 2's last runs were the first ones with
  `CS_TEST_BACKGROUND=1`, which never focuses the game, so DLSS never ran and the Upscale client never registered (c2q1, the last run
  without it, registered; every run after it did not, with the same build and config). cs-test now sets `CS_UPSCALE_UNFOCUSED=1` for
  background runs unless the config sets it (commit `fix(cs-test): upscale in background runs`). Three native-quick runs of stage 2's
  final build then registered DLSS before the shot and were clean by eye (k1q1-3).
- *Submission-thread time.* Re-measured on stage 2's build, `CS_ORG_SEGMENT_CLEARS=0` against on: busy 3.72 against 3.49 ms a frame
  (on cheaper by 0.23 ms; `FinishTicket` 3.47 against 3.20 ms, 51.9 against 38.8 tickets). The extra 0.66 ms of stage 2's single
  sample was outside the tickets: the present closure (`ORG.Host.SubmitThread.Closure`, `CS.Native.QueuedPresent`'s own time) was
  0.83 ms a frame in that run against 0.18 in its pair. That is the submission thread blocking in the present, and it moves from run to
  run (0.25, 0.37, 0.61 ms in this stage's samples), so frame-level submission-thread totals are compared with it taken out.

**Multi-tap segments** (`FrameAnchors.cpp`: `kDeclaredInfo`, `TapDeclared`, `OpenSegment`, `SubmitSegment`).
- A segment is a list of steps in frame order. A step is one hook: an anchor the segment holds wholly (it then has no epoch of its
  own), one or two named clients of an anchor that keeps its epoch for its other clients (a shared anchor), or another module's epoch
  (`SetForeignPasses`, `TapForeign`). The last step is the end tap.
- The segments are open: each submission records its entry barriers from the ledgers. Closed (stage 1's default), the validation
  layer reported layout mismatches for the normals targets (S2's hosted clear) and the shadow mask from the first frame DCLF drew:
  a closed execution's recording holds its entry barriers, made from the ledgers when it was recorded, and a neighbour that is
  open (DCLF's epochs, the anchors) left the resource in another state than when the kept recording was made. Closing to home
  (`CS_ORG_SEGMENT_REST=0`) showed it too; open showed none. Stage 1 and 2's pre-depth segment had the same exposure without a
  neighbour that changed. Closed segments make sense only once every neighbour is closed (`CS_ORG_SEGMENTS=3` keeps them for that).
- The frame's first tap takes the segment's ticket for the frame slot's copy (`SetEpochCopy`, then ORG's `HoldEpochTicket`, which now
  works for a pinned revision-driven epoch). It zeroes the copy's latch regions and closes the gates of every client of the segment.
  Every tap then commits its clients into that run inside `UseHeldRun`, and the end tap submits the ticket with the recording of the
  frame's description. A shared anchor's other active clients still run its own epoch.
- A shared anchor's named clients go to the segment only while its window is open: the frame declares it, its end tap has not
  fired, and every segment before it that the frame declares has ended. Outside the window (a menu's 3D scene drawing shadow masks,
  for example) they run in their anchor's epoch as before.
- The fields the taps use are read against the build the ticket was taken from: a build that attached between `Renderer::Begin`
  and the first tap renumbers the clients, so the first tap reads the fields again. A build that attaches between two taps gives the
  held ticket back (tap error `lost to a build`; none seen).
- Passes marked `atExecution` are the segment's late batch, recorded inline at its submission, unless they are GPU-gated (then
  ahead). Stage 1 recorded gated `atExecution` passes ahead; that is wrong for a pass that reads its run's slots when it records
  (the composite), so they are recorded at execution as on an anchor. A late batch of gated copies and clears alone keeps its list.
- The description gains `foreignOn` (a module epoch's passes on this frame) and `passes`: a hash of the revisions of the passes
  recorded ahead. A revision-driven recording is never checked against its passes' revisions at submission, so a pass whose recording
  depends on something beyond its bindings (light culling's light copy size) would otherwise be kept stale. It is read at Begin and
  again at the end tap.
- GPU work submitted while a segment holds its ticket runs ahead of the segment's work. The census counts it by why
  (`[ORG] frame segments: GPU work submitted while a segment held its ticket`): none in the steady scenes, and one native-context
  operation and one external-work submission in the first world frame of a load.

**The segments.**

| Segment | Steps (taps, in order) | End tap | Why nothing that must follow its work comes before the end |
|---|---|---|---|
| S1 pre-depth | EarlyPrepass | EarlyPrepass | one tap (stage 1) |
| S2 pre-main | the depth copy at Main::RenderDepth's end (`cs.engine.skeleton.depth-copy` at EngineDepthCopy), DepthDone, the shadow mask and its clear (`cs.shadow-mask.world`, `cs.engine.skeleton.frame@engine-draws` at EngineDraws), light culling (Light Limit Fix's epoch, held as a foreign step), Prepass (with its hosted normals and kMAIN clears) | Prepass | DCLF's Z-prepass epoch, which holds first person's depth too, is submitted before RenderDepth's copy; DCLF's main opaque epoch, the next GPU work that reads S2's results, is submitted after the Prepass hook. Between them no other epoch, external work or engine D3D11 draw is submitted (census) |
| S3 deferred | DeferredPasses (with CS's depth copy it hosts), DeferredComposite, PostComposite | PostComposite (reserved for the segment, so its hook always runs) | Deferred::StartDeferred runs the three hooks back to back after DCLF's main opaque epoch. The water stencil clear and water mask come after PostComposite |

The RenderDepth copy needs no first-person field: DCLF's first-person depth is a hosted part of the Z-prepass epoch now, so the copy
always follows both views' depth. The copy runs on every S2 submission whose description has it on (a copy has no indirect form).

What stayed outside:
- **Sky** (the main pass's sky batch, live draws). Its batch does not run in every world frame (interiors), so it cannot end S2,
  and after the end it keeps its ticket.
- **The water mask and stencil clear, the ImageSpace scopes (SSR, SSAO, snow), the kMAIN copy and the water surface.** They follow
  PostComposite, but the ImageSpace runs (stage 4) come between the water mask and the kMAIN copy. A segment ending at the water mask
  has no fixed end tap (the mask is not drawn without water in view), so S3 ends at PostComposite.
- **S0 (frame start).** Its reflection-sky anchor runs once per face, with live sky draws per face: each run's commit writes the
  run's latch regions and gates. In one segment run the second face's commit would overwrite the first's. S0 needs per-instance passes
  (face k's passes reading face k's region), a change to the anchor API and to the sky and cloud-shadow clients. Not built.
- **Fixed-set live passes as latched instances.** Not built. The shadow-mask draws are GPU-gated already and are recorded ahead in S2.
  The deferred composite and the IBL SH dispatch stay live, in the late batches of S3 and S2 (about 40 us of inline recording each
  a frame).

**Constants.** `SharedConstants` has eight upload points, each with a latch block of its own (a region is allocated only for runs
that use it). A segment puts an upload before each step whose clients read the shared buffers, numbered by the step, and the step's
tap writes that point's region with the values D3D11 would read at that hook. The graph orders each upload after the reads of the
one before it, so each member reads what its hook saw, as it did on its own anchor. The permutation block is the same: each step's
upload carries the permutation data as of its tap, which is what the anchor's commit captured before. No member reads per-draw
permutation data (they are compute passes and gated clears; the engine draws that need it are DCLF's).

**Light culling** (`ORGLightCulling.cpp`). With S2 on, its extension gathers no passes; S2 gathers them at their step
(`SetForeignPasses`), and `Execute` commits its latch through `TapForeign`. Its passes are off in a description whose light culling
is not active.

**ORG** (persistent-epochs.md, "Segments"):
- `HoldEpochTicket` for a revision-driven epoch with a pinned copy.
- Repeated alias activations are part of a predicted plan (`StatePrediction::activations`). A graph whose transients share memory
  activates the same backings at each execution, and was planned against the ledger every time: the deferred segment's admission was
  103 us a run (closed, in its first form), 8 us after (closed and open alike).
- A closed execution's exit commits are merged over mip runs, and seeds no longer push a batch onto the keyed commit path: the
  closed deferred segment's `CommitStates` went from 71 to 15 us a run.
- docs: a closed epoch's kept recording holds the entry barriers of its recording time.

**Alias pools.** The segments keep pools of their own. S2 has no transients. S3's pool is the deferred passes' 53.57 MB (67.92
independent). In the shared arena the deferred passes' memory also held Volumetric Lighting's 11.25 MB; with S3 in its own pool the
arena holds Volumetric Lighting alone. So the frame holds 53.57 + 11.25 + 1.33 = 66.15 MB of transients against 54.90 with segments
off: +11.25 MB at 1920x1080 output and 1280x720 render (measured from the alias plans), about +45 MB at 4K (estimated by area).
With `CS_ORG_SEGMENT_POOLS=0` (shared arena, measured) the deferred segment's admission is 29.9 us a run against 8.0 with its own
pool: its activations there are not the same every frame. The pools stay private: +11.25 MB for constant admission. The alternative
proposed in stage 2 (one placement for the whole frame's segments in their fixed order) was not built.

**Measurements** (perf-taa, native Vulkan, Perf-Fast, Tracy, one run each; measured unless marked).

| perf-taa (measured) | Stage 2 final (old ORG) | This build, S2 and S3 off | This build, on |
|---|---|---|---|
| fps between the marks | 125.5 | 123.3 | 125.2 |
| Tickets a frame | 38.86 | 38.67 | 33.92 |
| `vkQueueSubmit` calls a frame (all threads) | 37.7 | 37.6 | 34.3 |
| Batch lists a frame (census) | 44.9 | 45.7 | 40.9 |
| Inline lists recorded a frame (census) | 13.9 | 13.7 | 12.9 |
| Inline batches recorded a frame (`RecordInline`) | 21.8 | 23.7 | 22.9 |
| Render thread `SubmitEpoch`, ms a frame | 0.882 | 0.904 | 0.812 |
| Render thread busy, ms a frame | 3.22 | 3.32 | 3.19 |
| Submission thread busy, ms a frame | 3.49 | 3.54 | 2.94 |
| of it `FinishTicket` | 3.20 | 3.00 | 2.66 |
| Host thread busy, ms a frame | 2.77 | 2.67 | 2.09 |
| Barriers a frame, entry / body | 1229 / 1733 | 1197 / 1679 | 1202 / 1748 |
| Tap errors | 0 | 0 | 0 |
| Segment recordings in the steady window | 0 | 0 | 0 |
| Transient memory (alias plans) | 54.9 MB | 54.9 MB | 66.2 MB |

The frame rates are inside one sample's noise (121.8 to 125.7 fps across this stage's on samples). Per-epoch submission-thread
time moves by up to 100 us between runs for epochs this stage did not touch (DCLF's forward epoch, the reflection sky), so the
thread totals carry that noise; the per-segment table compares the same epochs' tickets.

| Per frame (measured; same build) | Pre-main segment | Its replaced tickets | Deferred segment | Its replaced tickets |
|---|---|---|---|---|
| Tickets | 1 | 5 (depth copy, DepthDone, one EngineDraws run, light culling, Prepass) | 1 | 2 (DeferredPasses, DeferredComposite) |
| Submission thread (`FinishTicket`), us | 87.5 | 222.3 (11.3 + 8.5 + 93.7 + 18.7 + 90.1) | 107.2 | 139.9 (66.0 + 73.9) |
| of it admission, us | 8.1 | 22.5 (3.1 + 2.6 + 6.4 + 2.3 + 8.1) | 8.0 | 11.6 |
| of it inline recording, us | 32.6 | 86.8 (1.4 + 46.9 + 38.5) | 28.9 | 30.6 |
| Render thread (`ExecuteEpoch`), us | 14.9 | 54.0 (5.6 + 3.8 + 17.9 + 8.3 + 18.4) | 5.7 | 20.9 |
| Barriers, entry / body | 35 / 23 | 51 / 16 (with one third of EngineDraws') | 53 / 84 | 60 / 69 |

The EngineDraws share is the difference between its three runs with the segments off and its two with them on (346.2 against
252.5 us; 72.1 against 54.2 us on the render thread). The deferred segment's first form (closed, before the ORG fixes) cost 265 us
a run: 103 us admission (alias activations planned every time) and 71 us of state commits.

**Checks** (the final build: segments open, private pools; `CS_TEST_BACKGROUND=1`):
- native-quick, Vulkan: 3 runs with DLSS registered before the shot (h3q1-3), clean by eye against stage 2's k1q3; 0 tap errors.
- native-d3d12-quick, the loading-thread run (`CS_ORG_LOADING_THREAD=1`), native-upscalers (DLSS, FSR, TAA, XeSS), first and third
  person (`pov first` and `pov third` shots), guest-quick (segments stay off in guest): passed, 0 tap errors, shots as before by eye.
- native-validation: the known messages only (the `SV_Target2` indirect-draw warning and the BackBuffer layout warning, each to the
  layer's limit of ten), 0 tap errors, no `lost to a build`. Dragonsreach's composite and shadow-mask variants by eye: as before.
  Recordings over the tour come from graph builds at the loads and one description change entering Dragonsreach: pre-main 44 copies
  (10 tap waits, 148 ms in all), deferred 52 (12 waits, 174 ms), with the validation layer on; none in a steady stretch.
- GPU work inside a held segment: one native-context operation and one external-work submission in the first world frame of a
  load, and the precipitation occlusion map's own clear once at a load. None in the steady perf window.
- native-river: shots match by eye (water, reflections). Open issue: both runs with the segments on logged host-skipped tap errors
  (51 and 56) for the water targets' clears during the frames of the `coc Riverwood` load: the clear waits for its ImageSpace host,
  and the engine-frame-clear epoch (the TAA mask's own ticket) is submitted first. The run with the two segments off had none; stage
  2's builds had 33 in one of their two river runs and 0 in the other. Not resolved; the steady river frames have none.
- The closed form of the segments (stage 1's default) failed validation: see "Multi-tap segments" above.
- ORG: the suite passes but the three known lease hangs and `HeadlessBenchmark`'s Debug timeout; the segment case covers held
  pinned tickets.

Not run: the RX 6600, a frozen-world capture frame (no tour reached one), a Bink start.

**What stage 4 should know.**
- The post chain cannot reach back into S3: the water mask, the ImageSpace scopes before the water surface (SSR, SSAO, snow), the
  kMAIN copy and the water surface sit between S3's end and DCLF's forward epoch. A declared post chain should take them as its first
  instances, with the water mask's presence as a description field (water in view), or the mask as a GPU-gated instance.
- Repeated hooks need instances. S0's reflection faces and ImageSpace's per-effect calls are both "the same anchor several times a
  frame". The segment model commits each tap into one run, so a client that runs twice in a segment needs per-instance latch regions
  and gate entries (an instance index in its commit, its passes made per instance).
- The present closure's time on the submission thread moves by up to 0.4 ms between runs: compare the tickets' time, not the thread
  total.
- A segment's late batch is recorded inline at every submission. The composite and IBL SH are the candidates for latched instances
  recorded ahead (2x2 composite variants by interior and normals parity); the normals parity must then be checked against the engine's
  choice at the tap.

## The declared frame, stage 4: the post description (2026-11-15, `org-native/phases`)

*The prediction, the check and the decision events described here are deleted (see "The post stage" below). `PostFields` remains as the post stage's structure read at Begin.*

Stage 4 of the declared-frame plan asked for the post chain as a declared segment (S4). This stage did the three items that come
before it: the river tap errors, per-instance taps, and the post description, checked against every world post chain the engine
draws. The declared chain itself (S4 drawing from declared instances) and S3's tail are not built; "What is left" says why and what
they need.

**The river tap errors** (commit `fix(org): age ImageSpace shapes by effect runs`). The 51 to 56 `host skipped` errors in the
`coc Riverwood` load frames had nothing to do with segments. They appear exactly in the river runs whose log says "ISWaterBlend into
rt1 is a shape this session did not register" (every river run since 2026-11-14 that had them; none of those without). The shape
manifest dropped a shape after sixteen sessions that did not draw it, whatever those sessions showed, so the water shapes (the water
blend into kMAIN_COPY, the displacement and wading shapes) fell out after a run of sessions without water. The next river session drew
them on first sight: each stayed with the engine until its late pass was built (about 60 frames), no ImageSpace run followed the
kWATER clear that waits for one as its host, and the TAA mask's own ticket went first. The run with segments off that had none ran
right after a river run, which had put the shapes back. Fix: a shape ages only in sessions in which its effect drew something (the
manifest keeps a session count per effect; an old manifest loads with every shape fresh). Checked with the manifest's session number
moved on by 20: all 107 shapes kept, 0 tap errors in the river run. In native mode the shape manifest goes away with the declared
chain; guest keeps it.

**Per-instance taps** (`FrameAnchors.h`: `ClientDesc::maxInstances`, `ClientDesc::instances`, `PassDesc::makeInstance`,
`TapInstance`, `InstanceEntry`). A hook that repeats inside one segment run (each ImageSpace effect call, each reflection face) is a
declared instance:
- A client with `maxInstances` above 1 has each pass made once per instance in a segment's graph (`<name>.i<k>`); its own anchor
  epoch makes instance 0 only.
- `instances`, read at Begin like `declared`, is how many do work this frame. It is part of the segment's description
  (`SegmentFields::instances`, one byte per client, in the hash and the diff), and the passes of the instances past it record only
  their barriers (`SetSkipWhen`).
- The segment counts each step's taps in its held run. The k-th tap is instance k: the client's commit reads `TapInstance()` and
  writes instance k's gate entries and latch regions (`InstanceEntry(k, perInstance, entry)`), never the anchor's single set.
- A tap past the declared count is a tap error (`instance not declared`) and its commit is not called. That includes a second tap
  of a client without instances in one segment run, which before this silently wrote over the first tap's regions. None of the runs
  below had one.
No client uses instances yet: they are for the declared chain (one instance per effect step) and S0 (one per reflection face).

**The post description** (`RenderGraph/Frame/PostChain.*`, `Description::post`, `PostFields`; switch `CS_ORG_SEGMENT_POST`, default
on where the ImageSpace hooks are installed, that is native mode). The world post chain is `ImageSpaceManager::RenderWorldPostProcess`
(SE 100430, AE 107148): the volumetric lighting, then `RenderPreTonemapComposites` (99024 / 105675), then, inside Upscaling's wrapper,
`RenderEndOfFrameEffects` (99023 / 105674). Its order is fixed in the engine's code; what varies is read from the engine's own state:

| Field | Engine state (SE / AE IDs; offsets are the same on both) | What it selects |
|---|---|---|
| volumetric | the VL switch (527940 / 414913) and the VL effect's active flag (manager +0x170, +0x08) | the VL apply and blur (Volumetric Lighting's port takes them) and the VL composite |
| sil, silParity | the SIL helper (manager +0x1d8): active +0x10; its counter +0x14, incremented and wrapped at 2 before each draw (100528) | ISSILRawInd then ISSILComposite |
| lensFlares | the lens flare helper (manager +0x180): active +0x30 (100481) | the IB lens flares (a group, below) and the composite form |
| (composite form) | volumetric and lensFlares (100493) | ISCompositeVolumetricLighting, ISCompositeLensFlare or ISCompositeLensFlareVolumetricLighting (manager +0x190, entries 0 to 2) |
| eof | gEnableEOFImageSpace (513299 / 391055) | whether the end-of-frame slots run at all (`CountActiveEOFEffects`) |
| slots | each slot effect's `IsActive` (vfunc 6), as 99023 calls it: world map (active flag), refraction (a global byte), FXAA, depth of field (active, manager +0x14c or the +0xb8 data, its +0x148), radial blur (manager +0x130 and a global byte), full-screen blur (+0x128), get hit (+0x12c) | which slot effects run, in slot order |
| ldr | usesLDR (manager +0x210) | slots 3 to 7 after the tonemap |
| hdr | the HDR switch (513463 / 391301) | the HDR effect, else ISCopy |
| effects11 | `State::PeekTonemapOwner` (new: the owner without resolving it) | Effects11 replaces the whole tonemap call |
| bloom | manager +0xcc (ImageSpaceData bloom radius), clamp(radius - 1, 0, 6) | which bloom composite (effects[23 + index]), drawn as its children (ISBrightPassBlur, ISBlur) |
| fade | manager +0x118 above 0 | the tonemap or its fade form (HDR children 2 and 3) |
| adapt | the adapted-luminance target (513465 / 391303) at or above 0, and its reset flag (528108 / 415053) off | the light adaptation draws the last level (HDR child 8) instead of the luminance shader |
| lumLevels | kHDR_DOWNSAMPLE0's size (RenderTargetManager, 524970 / 411451: a static object, not the pointer CommonLib reads), divided by 4 until 1x1, or 2x2 with the option (513488 / 391336) and without its override (528106 / 415051) | the luminance draws (HDR child 5, then child 6) |
| lumParity | the last level's parity (528109 / 415054), flipped by each HDR render | which target the last level writes (+2) |
| taa, taaMode, taaWater, taaParity | the TAA helper (manager +0x1f0): enabled +0x18, form +0x2c, water flag +0x38, history +0x34 (flipped per render unless the form is 2) | ISTemporalAA, its UI form or its water form; the history pair |
| dynres | the dynamic-resolution lock (525027 / 411509) off and a scale (525023, 525024 / 411505, 411506) not 1, and the upsample helper (manager +0x1f8) active | ISUpsampleDynamicResolution |
| screenshot | manager +0x211 | a copy after TAA |
| upscaler | CS: Upscaling's method is DLSS, FSR or XeSS and the window is usable | (what CS's wrapper does to taa and dynres) |

What the description could not settle yet, so the prediction treats it as a group (one or more draws of the group's shaders, any
order): the IB lens flares' own chain (100481, 889 lines of decompile, compute or raster downsamples by an option), and the depth of
field, full-screen blur, get hit, world map, radial blur and refraction slots' internal draws. For the depth of field the choice
(raster or compute blur, depth of field or distant blur, fogged or masked) is made inside its own Render: `BorrowTextures` (vfunc 4,
its first call) fills a five-step flag array at +0x98 that the Render then reads (`ImageSpaceEffectDepthOfField::Render`, SE
0x1412f90d0), so it is not readable before the effect runs without reimplementing that function. The volumetric lighting's group may
be empty: Volumetric Lighting's port takes its draws before the ImageSpace hooks.

**When it is read.** At Begin (the description) and at two decision events, which the census showed are needed:
- `RenderWorldPostProcess`'s entry (detoured): the slot effects' activity (the depth of field's state is set after Begin in most
  frames: 2110 of 2231 river chains), and TAA's form (set to 1 after Begin, back to 0 by each TAA render).
- `RenderEndOfFrameEffects`' entry (detoured; inside Upscaling's wrapper): TAA and dynamic resolution as CS's upscaler leaves them.
  Upscaling turns the engine's TAA on for every method in `Main_UpdateJitter` (after Begin), then to "method is TAA" just before
  the end-of-frame effects and off after them, and the dynamic-resolution lock moves the same way (the precipitation hook leaves it
  open). These are CS's own decisions: a later version can compute them at Begin from the upscaler's method.
`UI3DSceneManager::Render` (51855) calls the end-of-frame effects for a menu's 3D scene too; only the call inside the world chain is
checked.

**The check.** The ImageSpace hooks (BSImagespaceShader::Render, the explicit Render, DispatchComputeShader) note each shader the
chain draws by its index in `ImageSpaceManager::effects`. At the chain's end the sequence is compared with the prediction; a difference
is a tap error (`post not declared`) that logs both sequences. Prediction reads the composites' children where the engine renders a
list (the HDR effect's children, the bloom composite's), so a variant the engine selects by object is read, not assumed. Nothing is
drawn differently: this stage does not change rendering.

| Runs (this build, all native unless named) | World post chains checked | Matched | Tap errors |
|---|---|---|---|
| native-quick x4 (DLSS registered in each) | 2,611 | 2,611 | 0 |
| native-upscalers (DLSS, FSR, TAA, XeSS; the TAA run draws the engine's ISTemporalAA) | 1,833 | 1,833 | 0 |
| native-river x2 | 4,461 | 4,461 | 0 |
| native-validation x2 (Whiterun, inventory, map, a save and its load, Dragonsreach; validation layer on; only the known 20x `VUID-vkCmdDraw-None-09600`) | 7,488 | 7,488 | 0 |
| first and third person | 1,135 | 1,135 | 0 |
| the loading-thread run (`CS_ORG_LOADING_THREAD=1`) | 706 | 706 | 0 |
| native-d3d12-quick | 458 | 458 | 0 |
| guest-quick (no ImageSpace hooks: not checked) | - | - | 0 |

A steady perf-taa chain is: ISCompositeVolumetricLighting, ISHDRDownSample4, ISHDRDownSample4RGB2Lum, ISHDRDownSample16Lum x2,
ISHDRDownSample16LightAdapt, ISBrightPassBlur15, ISBlur15, ISHDRTonemapBlendCinematic, then the depth-of-field slot after the
tonemap (usesLDR): ISHDRDownSample4, ISDepthOfFieldBlurHCS, ISDepthOfFieldBlurVCS, ISDistantBlur. With DLSS the engine's TAA is off at
the end-of-frame effects; with CS's TAA method it draws ISTemporalAA.

**How often the description changes.** Counted at Begin against the same frame slot's description four frames earlier (a slot's
kept recording is what a change would record again; the parity fields alternate with the frame, so they stay equal per slot):

| Run | World frames | Frames whose description changed for their slot | Fields |
|---|---|---|---|
| native-quick x3 | 2,157 | 3 (one a run, the first world frames) | adapt, taa, taa form |
| native-river (walking along the river) | 2,232 | 7 | adapt 5, luminance parity 4, taa, taa form |
| native-upscalers (four method switches) | 1,834 | 5 | upscaler 4, taa form 5, taa parity 2, adapt, taa |
| native-validation (loads, menus, an interior) | 3,704 | 48 | slots 21, bloom 12, volumetric 12, adapt 11, luminance parity 8, taa 3, taa form |

The validation tour's changes come with its loads and Dragonsreach (bloom, volumetric lighting and the slots differ indoors). A
luminance-parity change for a slot means the HDR effect rendered a different number of times than frames between two of that slot's
frames (a load).

**Measurements** (perf-taa, native Vulkan, Perf-Fast, Tracy, one run each, the same DLL; off = `CS_ORG_SEGMENT_POST=0`). Nothing is
drawn differently, so the frame's work is the same; this shows the check costs nothing measurable.

| perf-taa (measured) | Check off | Check on |
|---|---|---|
| fps between the marks | 125.7 | 125.3 |
| Tickets a frame (census) | 33.91 | 33.93 |
| Render thread `SubmitEpoch`, ms a frame | 0.819 | 0.819 |
| Render thread busy, ms a frame | 3.34 | 3.28 |
| Submission thread busy, ms a frame (of it `FinishTicket`) | 3.19 (2.93) | 3.01 (2.70) |
| Host thread busy, ms a frame | 2.34 | 2.18 |
| Tap errors | 0 | 0 |

The submission and host differences are run-to-run noise (stage 3's notes: up to 0.4 ms on the submission thread); tickets, lists and
`SubmitEpoch` are unchanged, as expected with no change to the frame's graph work.

**What is left, and what it needs.**
- **The declared chain (S4).** The description now predicts every world post chain's effect sequence. Drawing from it needs each
  declared instance to be a concrete pass: an ORG pass declares its render targets, reads, samplers, blend state and viewport when it is
  built (docs/development/p5/imagespace.md, "Why the shapes stay passes"). The targets follow from the description (the HDR chain's are
  fixed by code, the source and destination alternate with each active slot as 99023's loop swaps them, the parities pick the pairs);
  the samplers, blend states and viewports are what each effect's SetupMaterial and Render bind, which today are read from the shadow
  state at the draw (`ReadShape`) and remembered in the shape manifest. Deriving them per effect from the effect objects is the
  remaining RE, and without it the declared instances would still be the manifest's shapes. The per-instance taps above are the
  mechanism the chain's commits use: instance k of the chain client is the k-th declared step.
- **S4's members and end tap** (designed, not built). With an upscaler, its evaluate (external work with a drain, inside Upscaling's
  wrapper before the end-of-frame effects) cannot run while a segment holds its ticket: work submitted meanwhile runs ahead of the
  held segment's earlier work on the GPU. So S4 is two segments when an upscaler evaluates: S4a from the Volumetric Lighting anchor
  through the pre-tonemap composites to the PostProcessing anchor (its end tap, reserved so its hook always runs), and S4b from the
  Upscale anchor through the end-of-frame effects and the Tonemap anchor to FrameGeneration. Without one, one segment. The upscaler
  stays external work unless it becomes a live pass in S4b.
- **RenderWorld scopes and S3's tail (item 5).** SSAO (RenderSSAO 100520 / 107245: its minify loop, temporal or not, and the
  composite form by fog and SAO switches and the portal graph), SSR and snow are not described yet; S3 still ends at PostComposite.
- **Guest** keeps the shape manifest and the per-anchor ImageSpace runs.

## The declared frame, stage 4b: the post chain from a pass-state table (2026-11-15, `org-native/phases`)

*Superseded by "The post stage" (recipe plan stage 3, part 3, below): the table, its generator, its check, the decision events and the cuts are deleted.*

Stage 4's second half: the world post chain drawn in two segments from a static pass-state table, checked against the engine at
every draw. Native mode, default on (`CS_ORG_SEGMENT_POST_DRAW=0` gives the chain back to the ImageSpace anchor; the table check
stays on with the post description; `CS_ORG_POST_TABLE_CHECK=0` turns off only the comparison of each draw with its row, and the draw
still happens from its row: see stage 0 of the recipe plan below). Guest mode is unchanged.

**Outcome.** perf-taa: 33.9 to 26.9 tickets a frame. The end-of-frame chain makes no ImageSpace-anchor ticket in a steady world frame
(13.0 to 5.0 ImageSpace tickets: the five left are the RenderWorld scopes, below). The two post segments cost the submission thread
38 us a frame against 184 us for the nine tickets they replace. 0 tap errors in every run, and in the steady window 0 segment
recordings, 0 cuts and 0 GPU work inside a held segment.

### The table

`src/RenderGraph/Engine/ImageSpace/PostTable.{h,cpp,inl}`. A row is a step and its variant (`PostChain::RowKey`: step, index,
parity, and for the steps that follow RenderEndOfFrameEffects' ping-pong the target read and the target written) with what its pass
is: the effect (its program), outputs, inputs, the samplers' address and filter modes, the engine blend state and a viewport rule
(`Full`: the whole output; `Dynamic`: the output scaled by the engine's dynamic-resolution ratios). 155 rows: 49 seen in the scenes
below and 106 implied by the RE of their steps (marked `// implied` until a run sees them).

- **Generated.** `CS_ORG_POST_TABLE_DUMP=1` logs one row per distinct declared step drawn (from the existing shape capture,
  `ReadShape`), and a conflict when a step draws a second shape or a viewport no rule gives. `tools/post-table.py <runs> [--keep]
  [--imply] --write` merges the rows, refuses two rows for one key, and writes `PostTable.inl` sorted by key. `--imply` adds, from
  rows seen: every luminance level and adaptation for 3 to 6 levels (720p to 4K), every bloom radius, the tonemap and its fade form
  from kMAIN or kMAIN_COPY into the destination or kIMAGESPACE_TEMP_COPY for each luminance target, TAA into the destination and with
  either sixth-sampler address, the depth of field's two temporary copies swapped, and refraction from kMAIN_COPY into kMAIN. The 26
  seen rows the rules also make are all equal to them.
- **Checked.** Each draw of a declared step is compared with its row (`CheckRow` in ImageSpace.cpp). Any difference, or no row, is
  a tap error `post state mismatch` naming the fields that differ. In the final runs every declared draw matched its row (28,751 of
  28,751 in the upscalers run; 6,000 to 9,000 in each native-quick run).
- **Fields the table needed.** Each was found as a row whose state was not constant, and each is a value the engine itself reads,
  now in `PostFields`:

| Field | Engine state (SE / AE) | What varied |
|---|---|---|
| eofSource, eofDest, eofSwap | RenderEndOfFrameEffects' arguments (99023 / 105674) and the manager's +0x60, read at its entry | where each step reads and writes; the source is kMAIN or kMAIN_COPY from frame to frame |
| dynScaled | the dynamic-resolution lock and scales, without the upsample's own flag | 99023 decides "final" from these alone |
| vlParity | the VL state object (getter 100297 / 107014), +0x88: the VL writes and the composite reads 43 + (~counter & 1) | the composite's input alternates between kIMAGESPACE_VOLUMETRIC_LIGHTING and its PREVIOUS |
| compositeAddress | the shadow state's first pixel sampler address, read at the composite's call (100493 does not set it) | wrap or clamp, by what drew before it |
| normals | Deferred's `forwardRenderTargets[2]` (the normals target bound at StartDeferred) | TAA reads kNORMAL_TAAMASK_SSRMASK or its swap, alternating out of phase with its history |
| taaAddress | the shadow state's sixth pixel sampler address, read at TAA's call | clamp for a few frames after an upscaler switch, wrap after |
| dofSteps, dofCompute | the depth of field's step flags (its bool array at +0x98) and the compute-blur option (513727 / 391684); its composites read at its first draw | the depth of field's draws |
| (luminance target) | the last level's target, 27 + levels - 1 + 2 x lumParity, as the parity of the adaptation, bright-pass and tonemap rows | rows that hold at any level count |

The decision events after Begin: RenderWorldPostProcess' entry, RenderEndOfFrameEffects' entry, a step's own call for the two sampler
fields (the rest of its part is predicted again from there), and the depth of field's first draw. Its Render picks its fogged,
masked or distant composites as it runs: the first world frame drew ISDistantBlur where the read at RenderEndOfFrameEffects' entry
said ISDepthOfField.

**What draws from the table.** The volumetric lighting composite (the VL-only form; the lens flare forms have no seen rows), the HDR
effect (downsample, luminance chain, adaptation, bright pass and blur at every radius, tonemap and its fade form), TAA (its plain
form), refraction (one draw of its child: `ImageSpaceEffectRefraction::Render`, SE 100962 / AE 107744), and the depth of field after
the tonemap with its compute blur (downsample, ISDepthOfFieldBlurHCS and VCS, ISDistantBlur, as `Step::Dof` rows read from its
children).

**What does not, and why.** A frame whose part holds any of these cuts that part's segment at its decision event and draws the part
on the ImageSpace anchor as before, counted by reason (`[ORG] frame segments: segments cut short`):

- the effects the description does not settle draw by draw yet: the IB lens flares (100481, two forms by an option), radial blur,
  full-screen blur, get hit and the world map (the validation tour's map cut the end-of-frame segment for 692 frames);
- FXAA (one shader, but off in every scene, so no row was seen), the depth of field's raster blur, pre-tonemap form and other
  composites, the copy that replaces HDR when it is off, the screenshot copy and the dynamic-resolution upsample: no scene drew them,
  so their rows are absent;
- TAA's UI and water forms (the port has no CS source for them and leaves them to the engine), and SIL (off in every scene).

### The segments

`FrameAnchors.cpp`, `kDeclaredInfo`.

| Segment | Steps | End | Why the end is safe |
|---|---|---|---|
| S4a post composites | VolumetricLighting (whole), ImageSpace (`cs.engine.imagespace.post.composites`), PostProcessing (whole, no clients) | the PostProcessing anchor's hook at Main_PostProcessing's start | VL and the composites run inside RenderWorldPostProcess before it; the upscaler's evaluate follows it |
| S4b post end-of-frame | Upscale (whole), ImageSpace (`...post.hdr`: slots before the tonemap, the HDR effect up to the bloom), Tonemap (shared), ImageSpace (`...post.final`: the tonemap draw, TAA, the LDR slots, the upsample), a point | RenderEndOfFrameEffects' return (`FrameAnchors::TapPoint`) | every chain draw's scope has flushed by then; FrameGeneration's capture comes after |

- **The upscaler's evaluate stays external work**, so S4 is two segments in every frame (one ticket more than needed without an
  upscaler). Streamline, FidelityFX and XeSS record it on the interop command ring, with their own resource-state contract and a
  drain. A live ORG pass would need each SDK to record into a list the segment owns, with its resources declared. Work submitted while
  a segment holds its ticket runs ahead of that segment's earlier work, so the evaluate must sit between two segments.
- **Three segment-only clients** (`ClientDesc::segmentOnly`): their passes are in the segments' graphs and never in the ImageSpace
  anchor's. One pass per row, gated, numbered by its row index (gate entry, latch region, `PassDesc::row`), ordered by the engine's
  step order (`PostOrderOf`: the LDR slots come after TAA though they sort before the tonemap). The HDR draws come before the Tonemap
  step and the tonemap draw after it, so Post Processing's and Effects11's passes sit between them in the graph.
- **Rows on and off.** `ClientDesc::rows` fills `SegmentFields::rows` (256 bits) from `PostChain::DeclaredFields()`: this frame's
  chain fields once its decision events ran, else the slot's last chain's. A pass whose row bit is clear records only its barriers.
  Read at Begin and again at the end tap; in steady frames they agree (0 recordings in the perf window).
- **Taps.** Each ImageSpace batch flush commits the post batches into the held run (`ClientDesc::repeats`: a client may commit at
  several taps of one run, each writing entries no earlier tap wrote). A segment with two steps at one anchor taps each step with its
  own clients. A draw of a part that draws from the table and is not its row draws nothing (a tap error).
- **Cuts** (`FrameAnchors::CutSegment`). At each part's decision event the part draws from the table only when every declared draw has
  a row with a built pass, for the sizes now, in a segment graph that holds it. Otherwise the segment is cut: what its taps committed
  is submitted, its later taps that frame do nothing, and its shared steps run their own epochs. The composites' cut waits for the
  composites' call, so the volumetric lighting is submitted with it first. A row made for another dynamic-resolution size registers the
  clients again (an async build) and cuts until that is built: two to four frames at the first world frame and at each upscaler switch.
- **Volumetric Lighting is a whole member.** As a shared step its anchor kept an epoch of its own that never ran. At the first upscaler
  switch that epoch ran once (its client was not yet in the segment's rebuilt graph) and failed with "Binding replacement requires a
  structural alias edit". That unregistered Volumetric Lighting and broke the TAA image in an earlier build of this stage.

### Measurements

perf-taa, native Vulkan, Perf-Fast, Tracy, one run each; off = `CS_ORG_SEGMENT_POST_DRAW=0`; measured unless marked.

| perf-taa | Off | On |
|---|---|---|
| fps between the marks | 124.2 | 123.3 |
| Tickets a frame (census) | 33.90 | 26.94 |
| Queue submissions a frame (batches, census) | 26.1 | 26.2 |
| `vkQueueSubmit` calls a frame (Tracy, all threads) | 34.3 | 34.3 |
| Batch lists a frame (lists in submissions) | 40.9 (122.1) | 34.9 (112.7) |
| Tickets joined by the next ticket's submission | 9.2 | 3.0 |
| Barriers a frame, entry / body (last census window) | 1189 / 1824 | 875 / 1243 |
| Closed passes on the GPU a frame (estimated: ImageSpace runs x 170 manifest passes, less those opened) | about 2,180 | about 840 |
| Render thread `ExecuteEpoch`, ms a frame | 0.888 | 0.694 |
| Render thread busy, ms a frame | 3.29 | 3.24 |
| Submission thread busy (of it `FinishTicket`), ms a frame | 2.96 (2.70) | 2.83 (2.48) |
| Host thread busy, ms a frame | 2.14 | 1.76 |
| Tap errors; recordings, cuts and GPU work inside held segments in the window | 0; 0, 0, 0 | 0; 0, 0, 0 |

The removed tickets were already joined into their neighbours' queue submissions, so the submission count does not move. The frame
rate difference is inside one sample's noise.

| Per frame (measured, same build) | S4a + S4b | Replaced: 8 ImageSpace chain tickets and the VL anchor's |
|---|---|---|
| Tickets | 2 | 9 |
| Submission thread (`FinishTicket`), us | 38.2 (17.4 + 20.8) | 184.2 (168.8 + 15.4) |
| of it admission, us | 9.5 | 62.5 (58.0 + 4.5) |
| of it inline recording, us | 1.4 | 1.2 |
| Render thread (`ExecuteEpoch`), us | 14.6 | 215.5 (205.4 + 10.1) |
| Barriers, entry / body | 42 / 119 | 353 / 684 (the ImageSpace anchor's 12.8 runs less its 5.1) + 8.5 / 17 |

Segment cost stays flat at this size: 17 and 21 us a ticket on the submission thread with 4 and 134 passes in the graphs, most of
them recorded as barriers only.

**Checks** (the final build, default on, `CS_TEST_BACKGROUND=1`; each with 0 tap errors of any kind and every declared draw matching
its row):
- native-quick, Vulkan: 3 runs with DLSS registered before the shot (q4-q6), clean by eye against the switch-off run of the same
  build; three earlier-build runs too.
- native-upscalers (DLSS, FSR, XeSS, and TAA drawing the engine's ISTemporalAA from the table): all four shots clean. An earlier build
  broke the TAA shot (the Volumetric Lighting failure above); the final build does not.
- native-river, native-d3d12-quick, the loading-thread run (`CS_ORG_LOADING_THREAD=1`), first and third person (`pov` shots),
  guest-quick (no post segments in guest): passed, shots as before by eye.
- native-validation (Whiterun, inventory, map, a save and its load, Dragonsreach; validation layer on): the known 20x
  `VUID-vkCmdDraw-None-09600` only; the map's radial blur and world map cut the end-of-frame segment (old path), as designed.
- GPU work inside a held segment: none in steady frames; at loads one native-context operation and one external-work submission (as
  in stage 3), the precipitation occlusion clear once, and at TAA switches the TAA mask clear (EngineFrameClear, still its own ticket).
- Not reached: underwater, DOF from dialogue, a hit for radial blur, a weather change by console, a frozen-world menu. Not run: the
  RX 6600. ORG was not changed, so its unit tests were not run.

**What is still reachable on the old path in native.** The shape manifest and the main ImageSpace client (170 passes): the RenderWorld
scopes (SSAO, SSR, snow, the water blend, the flow copies), the menus (local map, race menu, menu 3D) and every cut frame. Late
clients: a shape outside the manifest among those. Rising-entry flushes: in the main client's batches as before; the post clients'
batches never flush for order (their entries rise by row within a part).

### What is left

- Rows for the groups: the lens flares (RE of 100481, two forms by an option), radial blur, full-screen blur, get hit, the world map,
  FXAA (one run with FXAA on gives its rows), the depth of field's raster blur and pre-tonemap form, the copy, the screenshot copy, the
  upsample, and TAA's UI and water forms (which need CS sources).
- The chain's shapes can leave the manifest client in native once no frame cuts.
- **RenderWorld scopes and S3's tail** (not started): SSAO (RenderSSAO 100520 / 107245: its minify loop by level count, temporal or
  not by its option, the composite form by the fog and SAO switches) as rows with fields for those; SSR (100428 / 107146: the mask
  downsample, the ray march, the compute blur pair); snow (ISSnowSSS and the snow copy). Then S3's tail as a segment: the water stencil
  clear and mask (water in view as a field), those scopes, the kMAIN copy, the water surface and water-target clears, the water blend.
  These are the five ImageSpace tickets left a frame.

## The declared frame, stage 5: the RenderWorld scopes and S3's tail (2026-11-15, `org-native/phases`)

Parts 1 and 2 of the stage-5 list. Parts 3 to 6 (S0, the sun queries, the remaining post groups, the leftover clears) were not
started. Native mode; guest is unchanged.

### Part 1: the RenderWorld scopes as table rows

`PostChain::Scope` (Reflections, Ssao, Snow, SsaoComposite, WaterBlend). Each scope is predicted at its entry from
`WorldFields`, read from the engine there, and its draws are checked against the pass-state table as the post chain's are
(`CheckRow`; a scope that draws other effects than predicted is a `post not declared` tap error). The hooks: the reflections
pass and RenderSSAO detours, and the own-scope calls of ISSnowSSS, ISWaterBlend and the copy-subregion effect between RenderSSAO's
two calls.

| Field | Engine state (SE / AE) |
|---|---|
| which RenderSSAO call | the pass flag Main::RenderWorld (100424 / 107142) sets: 528126 / 415071 |
| saoLevels | the camera-Z level count, 528105 / 415050 |
| saoContrast | the contrast option 528124 / 415069; the level is the first whose kMAIN height >> (level - 1) is at most 16 |
| saoTemporal | the temporal raw AO setting 513569 / 391450 |
| saoHalf | the half-resolution set 528122 / 415067 |
| saoParity | the SAO helper's counter (manager +0x1c8, +0x54), plus one and wrapped at 2 as RenderSSAO does |
| saoForm | the helper's +0x50 and fog: 528125 / 415070 with GetPortalGraph (98984 / 105637) of the index 513241 / 390982 |
| ssr, ssrDownsample, ssrBlur | the reflections helper's active flag (+0x1c0, +0x10); 513568 / 391448 with the mode 513454 / 391287 (above 1, 4 compute); 528114 / 415059 |
| normals | GetImageSpaceRenderTarget_TAAMASK_SSRMASK_SWAP (100460): (~gFrameCounter & 1) \| 4 with SSR on (525008 / 411489), else 4 |
| snow | 513332 / 391105 and the snow helper's active flag (+0x1e8, +0x08) |
| waterHistory, waterAddress | gFrameCounter's parity; the shadow state's third and fifth sampler addresses at the water blend's call (it sets neither) |

Rows: 32 (30 seen in river, validation and upscalers runs with `CS_ORG_POST_TABLE_DUMP=1`, 2 implied raw-AO parities seen as
errors in a later river run). The table has 187 rows. Every scope of every run since matched its prediction, and every declared
draw its row.

### Part 2: S3's tail, a segment of its own (`SegmentId::World`)

A new segment after the deferred one, not a longer S3: the deferred segment's end (PostComposite) always fires, while the tail's
steps depend on water in view, and S3 had no room for the tail's steps. Steps: EngineDraws (the hosted stencil clear and
`cs.water.mask`), ImageSpace (`cs.engine.imagespace.world.scopes`: reflections, SAO, snow or its copy, the SAO composite), EngineCopy
(shared: the kMAIN copy, a gated late-batch copy), EngineDraws (`cs.water.surface`), ImageSpace (the hosted kWATER clear and
`cs.engine.imagespace.world.water`), and a point end. The end is `TapPoint` from `FlushRegions` before the next epoch that follows the
tail in every frame (DCLF's forward or first-person epoch, the sun queries, VL, post-processing, upscale, interface, present, the post
segments). Switch `CS_ORG_SEGMENT_WORLD` (default on; `=0` off).

- Water in view is not a description field: the water draws are late-batch passes and the kMAIN copy a gated late-batch copy, so
  a frame without water records nothing for them. The water blend's rows are always on (GPU-gated), so water entering view records
  nothing again. The scope rows follow the world fields of the slot's last frame.
- The water views commit more than once into one segment run (a batch of more than 64 draws is several runs): the water clients
  now `repeat`, appending to the run's draws (up to 256).
- A non-repeating client active at a second run of its step cuts the segment (`the <anchor> step ran again in one segment run`).
- A scope whose draws are not all drawable rows cuts the world segment at its entry (reason logged); only at startup and size
  changes was this seen ("made for another size").
- First build drew the scopes in table order: the SAO composite read kMAIN's copy before the snow copy wrote it (river shots fogged).
  The world rows now have order keys in Main::RenderWorld's order (`PostOrderOf`).

Measured (native-quick, Vulkan, `CS_ORG_EPOCH_STATS=1`): 20.0 tickets a frame in that scene; no ImageSpace, EngineDraws or EngineCopy
ticket in a steady world frame; the world segment 1.00 ticket, entry 36 and body 78 barriers. GPU work inside a held segment each
frame: the TAA-mask clear's own ticket (EngineFrameClear; part 6) inside the world segment. Recordings in steady windows: 0.

Checks (final build, `CS_TEST_BACKGROUND=1`, 0 tap errors in each, no `lost to a build`):
- native-quick x3, DLSS registered in each, clean by eye against stage 4b's q6.
- native-river: shots match stage 4b's r2 by eye (water, reflections, water blend); world cut only at the first frames.
- native-validation (Whiterun, inventory, map, save and load, Dragonsreach): known messages only; shots as before. Cuts: the
  post end-of-frame segment for the world map and radial blur (part 5).
- native-upscalers (4 registered), native-d3d12-quick, the loading-thread run, first and third person, guest-quick: passed.

Not done: the perf-taa off/on Tracy pair, the off-switch image run (the switch was not in the test registry; fixed after), the
per-segment cost table, the ORG tests (ORG unchanged), underwater, snow, DOF, a hit, a weather change.

## Recipe plan, stage 0: baseline and correctness (2026-11-15, `org-native/phases`)

The first stage of the plan that replaces the mirrored engine frame with a recipe of CS-owned stages: measurements of the current
build, one barrier trace, and four narrow fixes. Nothing was redesigned.

### Measurements

perf-taa, native Vulkan, Perf-Fast, Tracy, one run each, all measured. The off-switch runs use the build before the fixes. "After" is
the final build of this stage.

| perf-taa | Segments off (`CS_ORG_SEGMENTS=0`) | World segment off (`CS_ORG_SEGMENT_WORLD=0`) | Default | Default, after the fixes |
|---|---|---|---|---|
| fps between the marks | 126.0 | 124.5 | 123.0 | 122.0 |
| Tickets a frame | 51.8 | 26.9 | 20.0 | 20.0 |
| Queue submissions a frame (batches + uploads + leading uploads) | 35.6 + 2 + 1 | 25.4 + 2 + 1 | 22.0 + 2 + 1 | 21.9 + 2 + 1 |
| `vkQueueSubmit` calls a frame (RHI, all threads; plus 1 external-client submit) | 43.7 | 33.6 | 30.1 | 30.0 |
| Batch lists, inline lists a frame | 56.9, 13.9 | 34.9, 12.9 | 30.0, 13.0 | 30.0, 13.0 |
| Entry / body barriers a frame | 1356 / 1816 | 869 / 1235 | 628 / 852 | 630 / 852 |
| Render thread busy, ms a frame | 3.24 | 2.97 | 2.81 | 2.86 |
| Submission thread busy (of it the present closure), ms a frame | 3.80 (0.34) | 3.25 (0.35) | 2.48 (0.23) | 2.67 (0.21) |
| Host thread busy (of it retirement), ms a frame | 3.36 (2.23) | 2.23 (1.59) | 1.28 (1.05) | 1.49 (1.26) |
| GPU work inside a held segment, over the 900 frames | none | none | 1802 (901 clears, counted twice) | none |

Why each queue submission is apart from the one before (census, default): another queue 1.0, the same ticket 6.2, an earlier ticket
with a new wait 2.0, and an earlier ticket whose waits were already covered 12.8. In that last case the two could have been joined.

The world segment against the tickets it replaced (same build): one ticket of 169 us on the submission thread (129 us of it inline
recording of the water and late batches), against two EngineDraws runs (2 x 145 us), five ImageSpace runs (5 x 54 us) and the kMAIN copy
(45 us). On the render thread: 6 us against about 170 us. The other segments cost per ticket on the submission thread: pre-depth 69 us,
pre-main 89, deferred 117, post composites 17, post end-of-frame 40.

### The barrier trace

ORG's decision trace now names the execution of every event a host makes (`epoch`, `ticket`; decision-trace.md). A predicted
admission traces no barriers. So a steady frame was rebuilt from `ORG_TRACE=barriers,handoff,admission`:
- every execution's ledger commits (each frame) give the state it left each resource in;
- each epoch's latest plan gives its first use of each resource.
The script and the trace (`z0barriers.py`, `z0trace3.jsonl`) are in the session scratchpad, not in the repository.

One steady perf-taa world frame has 370 texture entry barriers on 166 textures: 296 state changes and 74 same-state barriers that only
order writes. There are also 216 buffer entries from another execution (buffer accesses are not traced). The census counts 628 entry
barriers, buffers included. By group:
- CS feature textures: 92 textures, 204 barriers;
- engine render targets: 57 textures, 117 barriers;
- engine textures made without a name on the native device (the G-buffer): 7 textures, 39 barriers;
- transients: 10 textures, 10 barriers.

What the barriers are for:
- **Data that crosses stages** (written in one execution, read in a later, different one): 65 textures, 132 barriers. 32 of them are
  CloudShadows' layers, written by the reflection prepass and both reflection-sky runs and read by the pre-depth segment and the sky.
  The rest: the G-buffer, kMAIN and the motion vectors from DCLF's main opaque epoch into the deferred segment; the shadow mask,
  Skylighting, screen-space shadows and terrain blending from pre-main into main opaque; terrain shadows, volumetric shadows and grass
  collision from pre-depth into the Z-prepass; the dynamic cubemaps from the deferred segment into the world segment; kMAIN, its copy and
  kMAIN_ONLY_ALPHA into the world segment; the refraction normals from forward into post; the TAA mask into post.
- **Re-transitions with no data between them**, between different executions or an epoch's own previous run: a resource that was read
  and is now written, written and written again (mostly same-state: kMAIN, the motion vectors and the G-buffer through every DCLF epoch),
  or read and read again in another layout.
- **An epoch's own cycle**: 52 barriers where a graph's last use of a resource differs from its first use (the deferred segment's SSGI
  and dynamic-cubemap scratch, the world segment's SSR and SAO targets). A whole-frame graph pays these too, unless each resource rests in
  the state of its first use.
- **The TAA-mask clear's ticket** (the frame-group clear graph) moves every frame-group target it could clear (kMAIN, the main depth,
  the normals, the shadow mask, the water targets) each frame, though only two clears do work: 12 entry barriers, and same-state barriers
  on 6 of them in the world segment after it.

### Fixes

- **A failed recording is fatal.** `SubmitSegment` asks for its description's recording once and waits once.
  - A request that comes back with an error (a pass's prepare or record threw, the graph failed, the host takes no requests), or does
    not arrive in two seconds, stops the game. A `logger::critical` line and `stl::report_and_fail` name the segment, the description's
    fields and the cause. A kept recording that failed is fatal where it is found (`RequestDeclared`, at Begin too).
  - A request that a graph build dropped (`EpochRecordingDropped`) is not a failure. It is asked for again at the next request. At the
    end tap the frame's segment work is dropped as `lost to a build`.
  - A recording that went stale while it was made is dropped with a tap error: an older build (`lost to a build`), or a slot that
    published a new version (`stale recording`, which replaces `no recording`).
- **`CS_ORG_POST_TABLE_CHECK=0`** turns off only the comparison of a draw with its row. The row is still looked up and the draw happens
  from it. A draw that differs from its built pass still draws nothing (`TakePost` checks the pass, for correctness).
- **The TAA-mask clear.** The world segment's reflections read kMAIN_ONLY_ALPHA (the SSR ray-march row, input 3). The clear's ticket was
  submitted while the segment held its ticket, so on the GPU the clear ran before the reflections. The skeleton now ends the world
  segment (`TapPoint`) before a frame-group run that clears the TAA mask or kMAIN_ONLY_ALPHA. The water passes use neither target. The
  census line counted each anchor run twice, because `Run` and `TryExecuteEpoch` both call `FlushRegions`. Now `Run` counts it and the
  submission's call does not.
- **Re-record lines.** Each segment logs up to 16 re-record reasons per 5 s window, then the number left out once the window passes.
  Startup no longer uses up the lines for the rest of the run.

### Paths that still drop a segment's work

- A build drops the recording request, attaches while it is recorded, or attaches between two taps (`lost to a build`).
- A slot publishes a new version while the recording is made (`stale recording`).
- `TryExecuteEpoch` returns Skipped (every published copy busy), EpochFailed (the segment's graph failed at submission) or Stopped (the
  runtime stops). The held ticket goes back.
- `OpenSegment` cannot take the ticket: the graph failed, or another error, which stops the runtime.
- Tap errors that draw nothing: a tap in a frame without the segment, a tap after the end, a client the description turned off, an
  instance past the declared count.
- A segment that holds its ticket at the frame's end without its end tap: released at the next Begin.
- A cut (`CutSegment`): later taps that frame do nothing, and their parts draw on the anchor.
- Hosted clears and copies whose host skips or misses them (`host skipped`, `host missing`).

### Checks

- native-quick on Vulkan: 3 runs with the upscaler registered, clean by eye against stage 5's final run. One run with
  `CS_ORG_POST_TABLE_CHECK=0` matches the default.
- native-d3d12-quick, native-river, native-upscalers (4 registered), native-validation (20x `VUID-vkCmdDraw-None-09600` and the
  BackBuffer layout warning only), the loading-thread run and guest-quick: passed, shots as before by eye, 0 tap errors, no new `[E]`
  kind.
- native-river with `CS_ORG_SEGMENT_WORLD=0` against the default, on the build before the fixes: the same by eye.
- The fatal path did not fire.
- GPU work inside a held segment: none in steady frames. At loads one native-context operation and one external-work submission, and
  in the first world frames ImageSpace runs while the world segment is cut, as before.
- ORG tests: all pass but the three known lease hangs and `HeadlessBenchmark` (not run).

### What later stages should know

- About a third of the texture entry barriers carry data across stages. The rest are layout round trips and cycles that a recipe with
  planned first-use states can remove.
- Predicted admissions trace nothing. Use the ledger commits to see a steady frame.
- The frame-group clear graph is planned with every target it could clear, so its ticket touches 12 targets each frame for two clears.

## Recipe plan, stage 1: frame-boundary discipline (2026-11-15, `org-native/phases`)

The rule: a frame runs entirely on the schedule that was current at `Renderer::Begin`. A schedule is the set of built graphs,
resource versions and kept recordings a frame uses. A change is asked for, recorded on the host's thread, and switched in at the
first frame boundary after its recording is ready. Nothing is described, recorded, checked or waited for in the middle of a frame.
Native mode with segments; guest is unchanged.

### The schedule

`FrameAnchors.cpp`: `State::schedule`, `Schedule`, `BeginFrame`.
- Per declared segment and frame slot: the description the slot's frames run on, its recording, and the snapshot of the segment
  graph it was recorded on.
- At Begin each segment makes the frame's own description (`DescribeDeclared`: clients' `declared` predicates, rows, instances, module
  steps, the passes' revisions, the version generation). If it equals the slot's schedule, the frame runs on it. If not, its
  recordings are asked for (this slot and the next three, `RequestEpochRecording` for one copy each), and the slot switches to it at
  this Begin if the slot's copy is already recorded, else at a later Begin of the slot. Until then the slot keeps its old schedule.
- A schedule is dropped at Begin when its build is no longer attached, or when a slot it binds has a newer version (the segment's
  version generation moves, so the description is new).
- The frame keeps what Begin chose: `frameRecording`, `frameBuilt`. The taps and the end tap use them. `SubmitSegment` submits the
  frame's recording; it no longer describes again, checks `StaleVersions`, asks for a recording or waits (`AwaitDeclared` is gone).
  A build attached or a version published after Begin is a mismatch (below), not a re-record.
- Every switch is logged with what changed and how many frames after the description was first asked for
  (`[ORG] schedule: the <segment> segment switches in frame N (slot s, k frames after it was asked for): <diff>`), 16 lines a
  segment in 5 s, the rest counted and logged when the window passes.
- **Lagging.** While a slot runs on an older schedule than its description, a client the description has on and the schedule off
  draws nothing at its tap (counted as lagged work, not a mismatch).
- **The post and world rows.** The rows are read at Begin only, never again at the end tap. A segment's rows are the union of what
  every frame slot's last chain (and last scopes) drew, so a variant the engine picks without a four-frame period (the depth of
  field's output target, TAA's sixth sampler after a switch) is on already when it comes back. The composites' rows are all on (the
  composite's first sampler is whatever drew before it, which flips from frame to frame), as the water blend's were. Rows a frame
  does not draw do nothing (GPU-gated). The decision events (`ChainStart`, `EndOfFrameStart`, `ScopeStart`) no longer change the
  frame's description: they predict the frame's draws, and a part whose rows the schedule has off is cut there (`RowScheduled` in
  ImageSpace.cpp) and drawn on the ImageSpace anchor that frame. A step decided at its own call (`NoteShader`: TAA's sampler, the
  depth of field's composites) whose row is off cuts the segment at that draw (`TakePost`): the part's earlier draws are committed
  first (`Flush`), the rest draw on the anchor. The image is right in both cases; the slot's next schedule has the row.
- `EndOfFrameStart` predicts TAA's sixth sampler and the depth of field's steps from the slot's last chain instead of 0 (they are
  decided at the step's call); before, the end-tap re-read hid that the Begin prediction was wrong in every TAA frame.
- **Absent.** A slot with no schedule on the attached build (the first world frames, and the frames after a build of the segment's
  graph until its first recording for that slot is done) runs without the segment. Its fields carry graph 0. Its taps draw nothing
  (counted), it holds no later segment's window, and `CanRun` is false for the clients of anchors it holds wholly, so those clients
  and the skeleton's hosted clears keep their own paths (the engine's, D3D11's or the anchor's ticket). Shared anchors run their own
  epochs; the post chain draws on the ImageSpace anchor. What is not drawn in such a frame: the work of the wholly held anchors whose
  clients do not check `CanRun` (see the measurements for how many frames this is).

### Builds attach before Begin

Engine order (Ghidra, SE 0x1405b1020, Main's frame): `Renderer::Begin`, then the decal manager and UI work, then `Main::Draw`
(0x1405b1710), whose first call is where Drawcall Limit Fix's scene frame starts (`Main_Draw_Early`, 35560 + 0xD3). DCLF's build
point (`IndirectDraws::BuildPoint`: its builds handed out, the build manager's finished builds attached, `RefreshGraphConditions`)
ran there, after Begin. It needs nothing from `Main::Draw`: the graph conditions read the cell and the toggles, and the revision
inputs it posts are latest-wins. Native mode with segments now runs it from the frame driver at Begin, before the descriptions
(`DCLF::BuildPointAtFrameStart`), for a frame that draws the world; the scene-frame start skips it for that frame. Guest keeps it at
`Main::Draw`. The other attach points are the frame's end (`SealPresented`, `Attach::All`), DCLF's own wait for a snapshot and
`BuildOwnerEpochsNow`, which attach only DCLF's epochs, and setup (`BuildPending`) before the first world frame.

### Versions

- DCLF's versioned buffers are grown by the snapshot builder off the render thread, for the snapshot it builds, and adopted whole
  with that snapshot's revision at DCLF's frame start (`AdoptCurrentSnapshot`, `NamedVersions::AdoptAll`, then `NoteNewVersions`):
  a frame never needs more capacity than the snapshot it draws was built with, and its versions change with its recordings. That
  adoption is after `Renderer::Begin`, but before any DCLF epoch of the frame and on DCLF's own epochs only.
- The segments do not bind DCLF's buffers: their recordings check `EpochRecordingVersionsCurrent` (stage 1 of the declared frame),
  not the host-wide `NoteNewVersions` count, and no run below dropped a schedule for versions.
- A version a segment binds that is published in the middle of a frame (an engine target made again, a feature twin resized) is a
  mismatch at the end tap; at the next Begin the schedule is dropped and the new version recorded.

### Sizes decided up front

Light culling's light copy grew at its tap (powers of two), which changed a recorded pass's revision. With a segment holding the
passes, the copy now always moves the whole capacity (1024 lights), and a new cluster grid (a new render size) is applied at the next
frame's start (`ORGLightCulling::BeginFrame`, called from its declared predicate, which the pre-main description reads at Begin).
The `passes` hash is read at Begin only.

### The mismatch policy

`Frame::Driver::NoteTapError` is the one place: every tap error kind (a tap the schedule does not have, a client it has off, a
missing end tap, `host skipped`, `host missing`, `post not declared`, `post state mismatch`, a build or a version after Begin).
- Development builds (every build but the Release profile, `CS_RELEASE_PROFILE`: Dev-Fast, Perf-Fast and the test stages) stop with
  `stl::report_and_fail`, naming the frame, its kind and slot, the segment, the tap and the difference.
- The Release profile logs one line per kind for the session and counts the rest; the work is not drawn.
- `CS_ORG_MISMATCH_FATAL=0/1` overrides. A cut is not a mismatch; cuts are counted as before.

### Clients declare

Every segment client's `declared` predicate is read at Begin; null means on. The clients' `active()` at the tap still decides whether
the commit runs, but that never changes the frame's recording: an on client that is idle at its tap leaves its GPU-gated passes
closed. IBL now declares its passes from the scene setting it already reads (`IsDisabledForCurrentScene`). The others' activity is
known only at their hooks (data staged by the hook: Terrain Shadows' and Cloud Shadows' pending work, SSGI's and the composite's
frame inputs, Volumetric Lighting's run, the post batches, the water views), so they stay on whenever registered (Terrain Blending
and Volumetric Shadows had predicates already).

### Measurements

perf-taa, native Vulkan, Perf-Fast, Tracy, one run each, measured. Before: stage 0's final run (`z0fix`, the build this stage started
from). After: this stage's final build.

| perf-taa | Before | After |
|---|---|---|
| fps between the marks | 122.0 | 123.2 |
| Tickets a frame | 19.96 | 19.94 |
| `vkQueueSubmit` calls a frame (RHI; plus 1 external) | 30.04 | 30.01 |
| Batch lists a frame | 29.95 | 29.94 |
| Entry / body barriers a frame | 630 / 852 | 630 / 852 |
| Render thread busy, ms a frame | 2.86 | 2.76 |
| Submission thread busy, ms a frame | 2.67 | 2.50 |
| Host thread busy, ms a frame | 1.49 | 1.26 |
| Segment tickets on the submission thread (`FinishTicket`, us a frame: pre-depth, pre-main, deferred, world, post composites, post end) | 68, 90, 121, 234, 17, 44 | 67, 88, 112, 205, 17, 38 |
| Segment runs on the render thread (`ExecuteEpoch`, us a frame, the six summed) | 57 | 58 |

The thread and frame-rate differences are inside one sample's noise; the frame's work is the same.

The census (`CS_ORG_EPOCH_STATS=1`, final build, `CS_ORG_MISMATCH_FATAL` on, as in every run below):

| Run | Render-thread waits for a recording | Recordings asked for after a Begin | Tap errors | Absent frames (worst segment) | Frames on a lagging schedule (worst) | Cuts (all segments) |
|---|---|---|---|---|---|---|
| perf-taa, the 900 marked frames | none (no wait in the code) | 0 | 0 | 0 | 0 | 0, and 0 recordings, 0 switches |
| native-quick | none | 0 | 0 | 7 (deferred) | 4 (post end) | 5 |
| native-river | none | 0 | 0 | 11 (deferred) | 6 (post end) | 10 |
| native-upscalers | none | 0 | 0 | 7 | 6 (post end) | 15 |
| native-validation | none | 0 | 0 | 15 (deferred) | 37 (post end) | 724 (719 the inventory's and map's post groups, as before: 693) |

- Recordings are asked for only at Begin and switched in only when done, so each was made on the host's thread before its switch. The
  switches' lag, in frames since a slot began to wait for the description (validation tour, all segments): 0: 12, 1: 47, 2: 45, 3:
  37, 4-7: 30, none later. In the other runs no switch waited more than 7 frames.
- What switches: each segment's first schedule on each build (the loads' and startup's builds), the post end-of-frame rows (luminance
  levels, adaptation, the depth of field in menus), and pre-main and pre-depth client bits in Dragonsreach (Volumetric Shadows off
  without directional shadows).
- Absent frames are the frames right after a segment graph's build (startup and the loads' builds), 1 to 3 per build. In them the
  segment's wholly held anchors report `CanRun` false, so their clients and the hosted clears keep their own paths, shared anchors
  run their own epochs and the post chain draws on the ImageSpace anchor. No shot showed a missing effect; the absent frames are under
  the loading menu's fade.
- No mid-frame re-describe or re-record exists any more: `SubmitSegment` takes the frame's recording, `OpenSegment` fails (a
  mismatch) on a build attached after Begin, and `lost to a build` and `stale recording` were 0 in every run.

### Checks

One run each, `CS_TEST_BACKGROUND=1`, mismatches fatal, the final build: native-quick (DLSS registered), native-d3d12-quick,
native-river, native-upscalers (all four registered; DLSS, FSR, TAA and XeSS shots), native-validation (the known 20x
`VUID-vkCmdDraw-None-09600` and the BackBuffer layout warning only), the loading-thread run, first and third person, guest-quick:
all passed with 0 tap errors, shots as stage 0's by eye. The fatal path fired twice during development, both findings fixed before the
final runs: a later segment's window opened while an earlier one was absent (a hosted clear then saw its host skipped), and the
composite's sampler predicted from the slot's last chain did not take its call-time correction before its row check (the composite
keeps the read at the chain's entry, and its rows are all on). ORG was not changed, so its unit tests were not run.

### What stage 2 should know

- A build still makes its segment absent for a frame or two: a recording can only be made for an attached graph. Recording a built,
  not yet attached graph (`BuildEpochGraph`'s result) would let the build and its schedule switch in together.
- The decision events and cuts remain for the post table; stage 3 removes both. The rows union is a stopgap for engine choices the
  description cannot see ahead.
- Clients' `active()` at the tap remains for staged hook data. A recipe pass that takes its inputs from the frame inputs block has no
  such predicate.
- DCLF's own adoption (`AdoptCurrentSnapshot`) is after Begin but before any DCLF epoch; moving it to Begin needs the frame inputs it
  posts at `Main::Draw`'s start.
- TAA and dynamic resolution are not computed at Begin from the upscaler's method. The schedule takes them from the slots' last
  chains (what CS's upscaler left), which equal the frame's in steady play; an upscaler switch cuts the post end-of-frame segment for a
  few frames while the new rows record (native-upscalers: 9 to 10 cuts for four switches). Computing them up front belongs with the
  CS-owned post stage.

## The recipe (recipe plan, stage 2; 2026-11-15, `org-native/phases`)

Native mode with stages on (`CS_ORG_SEGMENTS`, default). Guest mode keeps its per-anchor path.

### What a recipe is

`src/RenderGraph/Frame/Recipe.{h,cpp}`. The world frame is a fixed list of stages, in the shape of BasicRenderer's
`PipelineRecipe`: recipe order is pass order.

| # | Stage | Owner | Epochs | Commit point |
|---|---|---|---|---|
| 1 | start | tickets at hooks | decal queries, reflection prepass, cube clears, DCLF reflection, face sky | each hook |
| 2 | shadows | DCLF | shadow clears' anchor, shadow view, sky occlusion | DCLF's shadow views |
| 3 | pre-depth | CS | `DeclaredPreDepth` | the EarlyPrepass hook |
| 4 | depth | DCLF | Z-prepass, first-person depth | Main::RenderDepth |
| 5 | pre-main | CS | `DeclaredPreMain` | the Prepass hook |
| 6 | opaque | DCLF | sky's anchor, main opaque | the main pass |
| 7 | deferred | CS | `DeclaredDeferred` | the PostComposite hook |
| 8 | water (merged into deferred+water by stage 3) | anchors' tickets (CS with `CS_ORG_SEGMENT_WORLD=1`) | `DeclaredWorld`, or the EngineDraws, ImageSpace and EngineCopy anchors | just before the forward epoch (`FlushRegions`), or each hook |
| 9 | forward | DCLF | frame-group clears' anchor, forward, sun queries | the blended draw |
| 10 | first person | DCLF | first person | Main::RenderFirstPersonView |
| 11 | post | CS (table) | the two post stages, the Tonemap and FrameGeneration anchors | PostProcessing, then RenderEndOfFrameEffects' return |
| 12 | UI | tickets at hooks | movie tiles, interface, present, overlay, HDR output, capture | each hook |

A CS stage is a FrameAnchors declared segment: one revision-driven epoch, one kept recording per frame slot, the schedule of
stage 1 of this plan, one ticket a frame. A feature contributes passes as before (`ClientDesc::passes`); the stage that holds its
anchor holds them, ordered by step and order key, and the stage's graph orders them by what they declare.

Anchors that run at several places a frame (ImageSpace, EngineDraws, EngineCopy, the frame-group clears) are placed where they
run in a steady world frame. Their real order varies, so the check below treats their place as nominal.

### The check and the print

Each epoch's schedule is read as its graph was built (`org::RenderGraph::TraceSelectedSchedule`): on the build thread when the
build manager builds it, taken at its attach; else, once per build, on the render owner at `Renderer::Begin` (setup builds), at
most two a frame. When every epoch of the recipe has a trace of its current build, the recipe is checked and printed:

- Within a stage, its graph orders its passes; the check is between stages.
- A read is fine when the reading stage or an earlier one writes the resource. A read of something no graph writes comes from
  outside the graphs (uploads, files, the engine), counted.
- A read whose writers are all in later stages is the previous frame's. It must be listed in `kPreviousFrame` (12 names, each
  checked by hand: the refraction normals, underwater mask, water displacement pair and flow map, kMAIN_ONLY_ALPHA and kMAIN_COPY
  written by forward or post; the sky IBL, Terrain Blending's blended depth and the dynamic cubemaps read by the start stage;
  Volumetric Lighting's depth copy read by the menu scenes' fog).
- DCLF's stages are counted apart: their graphs declare the whole binding table their shaders may read (the light lists, the
  shadow mask, the luminance chain).
- Anything else is a finding: fatal in development builds, logged in release (`CS_ORG_RECIPE_FATAL=0/1` overrides; default as
  `CS_ORG_MISMATCH_FATAL`).

The print (`[ORG] world recipe: <n> <stage> (...): pass [reads ...; writes ...]; ...`) is one line per stage, logged when the
recipe's passes change (12 times a session at most), and `CS_ORG_EPOCH_STATS=1` adds a census line.

### Frame inputs and the one commit

ORG gives each revision-driven epoch one staging run per published copy (`GraphHost::EpochStagingRun`): a lease index no run
opens and no recording reads.
- At Begin each present CS stage zeroes its frame slot's staging run, clears every run gate's record of it and closes its
  clients' gates there.
- A tap (a step's hook) runs its clients' commits with `CurrentRun()` the staging run (`GraphHost::UseRun`). Latch values, gate
  words, shared constants and `RunSlots` values all land there. No ticket is taken.
- The stage's last step is its commit. `SubmitSegment` pins the slot's copy, and inside the ticket's `beforeSubmit` copies the
  staging run onto the copy's run: `LatchBlock::CopyStableRun` (latch regions), `RunGate::CopyOpenAll` (every gate's open
  record, including gates a client does not list), `RunSlotsRegistry::MoveAll`. Then the kept recording is submitted.
- A staged tap still counts as run for its callers: its executed callbacks run at the tap, as an early tap of a held ticket did.
- `FrameAnchors::StageInputs(id, write)` stages data for a stage client from a call that is not its step's hook (an engine clear
  call, below).

Why each commit point is complete and nothing that must follow the stage is submitted before it: the same hooks as before (the
segments' table in the declared frame's stage 3 and 5). What runs between a stage's first tap and its commit is counted
(`GPU work submitted between a stage's first tap and its commit`): none in steady frames.

The staged values are plain bytes and handles. A later render thread can keep a staging run per slot as its input block.

### Clears and copies

| Clear or copy | Now, a native world frame |
|---|---|
| kSHADOW_MASK, the normals targets and kMAIN (pre-main) | a clear pass of the stage; the engine's call stages its gate entry and colour into the stage's frame inputs (`StageInputs`) |
| The water stencil, kWATER_1/2 (water) | the same with the water stage on; with it off (the default) the frame-group anchor's ticket |
| Depth copy at RenderDepth's end (pre-main) | a declared, ungated copy pass of the stage; the engine's call does nothing more |
| CS's depth copy in Deferred::DeferredPasses (deferred) | a declared, ungated copy pass, first in the stage |
| kMAIN into its copy (water) | unchanged: a gated copy at the EngineCopy anchor (a step of the water stage when it is on) |
| Shadow slices, main depth | unchanged: gated clears hosted at the start of DCLF's shadow-view and Z-prepass graphs |
| Reflection faces | unchanged: hosted at the face sky's anchor (start stage) |
| TAA mask, kMAIN_ONLY_ALPHA, precipitation occlusion | unchanged: the frame-group or shadow-group anchor's ticket |

`AllowedBetween` is gone (only the stage hosts used it); the copies' hosted CPU record (`hostedPending`) is gone. The hosted
record, `host skipped` and `host missing` remain for the DCLF and reflection-face hosts. Other frame kinds keep the per-anchor
path.

### The water stage is off by default

*Superseded by recipe plan stage 3 (below): the darkness did not reproduce; the water stage is on and merged into the deferred stage.*

With its taps staged, the water stage loses interiors' ambient light. Dragonsreach's mean brightness in the validation tour:

| Build and switches (validation tour, measured) | Dragonsreach |
|---|---|
| guest mode | 101.3 |
| stage 1 of this plan (held tickets) | 89.8 to 98.0 over 6 runs |
| staged, deferred and water merged | 39.6, 40.3 |
| staged, merged, `CS_ORG_SEGMENT_CLEARS=0` | 39.5 |
| staged, merged stage off | 92.7 |
| staged, split, before the gate fix | 47.1 |
| staged, split, after the gate fix | 63.4, 44.8 (deferred off), 59.3 (`CS_ORG_SEGMENT_CLEARS=0`) |
| staged, water stage off | 92.9 |
| staged, deferred on, water stage off (the default) | 93.0 |

Exteriors match in every run. One cause was found and fixed on the way: the deferred composite's run gate is not in its client's
`gates`, so its open record never reached the submitted run (`RunGate::CopyOpenAll` now copies every live gate's). The water stage's
remaining fault is not found: no GPU work runs between its taps and its commit, and the clears are not it. Its clients are the
water draws, the RenderWorld scopes drawn from the pass-state table (SAO, SSR, snow, the SAO and fog composite), the kMAIN copy and the
hosted clears; the scopes' commits (`ImageSpace.cpp`, `CommitBatch`) are the next suspects. So `CS_ORG_SEGMENT_WORLD` defaults off
(its anchors keep their tickets, as before the declared frame's stage 5) and `=1` turns it on.

The deferred and water work can be one stage once the water stage is right: every tap has staged by the forward epoch, and the census
saw no GPU work between them. Merged, it showed the water stage's fault, so it was split.

`CommitBatch` also keeps the sources of a run until the run's lease index runs again; a staging run (and a held copy run before it)
names the same run every frame, so those lists only grow. Not fixed here.

### ORG

- `GraphHost::EpochStagingRun`, `GraphHost::UseRun`, `LatchBlock::CopyStableRun` (persistent-epochs.md, "Segments"); test in
  `PersistentVulkanHostTests` (Segment).
- `RenderGraph::TraceSelectedSchedule(epoch)`: an epoch's schedule traced on demand.

### Measurements

perf-taa, native Vulkan, Perf-Fast, Tracy, one run each. Before: stage 1's final run (measured then, the same machine). After:
this stage's final build (water stage off by default).

| perf-taa | Before (stage 1) | After |
|---|---|---|
| fps between the marks | 123.2 | 120.9 |
| Tickets a frame | 19.94 | 28.93 |
| `vkQueueSubmit` calls a frame (RHI; plus 1 external) | 30.01 | 34.80 |
| Entry / body barriers a frame | 630 / 852 | 899 / 1205 |
| Render thread busy, ms a frame | 2.76 | 3.59 |
| Submission thread busy, ms a frame (of it the present closure) | 2.50 | 3.69 (0.58) |
| Host thread busy, ms a frame | 1.26 | 2.34 |
| Recordings and schedule switches in the steady window | 0, 0 | 0, 0 |

The ticket, barrier and thread differences come from the water stage being off: its work is back on five ImageSpace, two
EngineDraws, one EngineCopy and two more frame-group clear tickets, as before the declared frame's stage 5 (stage 0 measured that
configuration at 26.9 tickets and 869 / 1235 barriers). The stages themselves cost what their segments did:

| Per ticket (after, measured) | Submission thread `FinishTicket`, us | of it admission | of it inline recording | Render thread `ExecuteEpoch`, us | Stage 1's segment (submission, us) |
|---|---|---|---|---|---|
| pre-depth | 71.4 | 7.8 | 2.6 | 21.9 | 67 |
| pre-main | 96.0 | 9.5 | 37.6 | 30.7 | 88 |
| deferred | 108.7 | 8.7 | 38.2 | 12.6 | 112 |
| post composites | 19.9 | 5.7 | 1.3 | 16.9 | 17 |
| post end-of-frame | 21.2 | 6.0 | 0.0 | 19.9 | 38 |

The commit's copy (`CS.Stage.CopyLatch`, `CopyGates`, `MoveSlots`) is a few microseconds a stage once the staging run lives in
host memory; in the GPU-visible arena it was 0.8 ms a stage (88 fps).

### Checks

The final build, one run each, `CS_TEST_BACKGROUND=1`, mismatches and recipe findings fatal: native-quick (DLSS registered),
native-d3d12-quick, native-river, native-upscalers (four registered), native-validation (the known 20x `VUID-vkCmdDraw-None-09600`
only; Dragonsreach 97.0, inventory and map equal to stage 1's), the loading-thread run, first and third person, guest-quick: all
passed, 0 tap errors, 0 recipe findings. The steady recipe: 329 passes, 8 inputs from the previous frame, 81 from outside the graphs.

The fatal paths fired during development, each a finding fixed before the final runs: the recipe checked before every epoch had a
trace (a partial recipe misses writers), anchors that run at several places placed in one stage, and the cross-stage reads now in
`kPreviousFrame`.

### What is left of the old segment API, and who uses it

- Shared steps (the EngineDraws, ImageSpace, EngineCopy and Tonemap anchors' named clients), the point steps (the water stage's and
  post end-of-frame's ends), `repeats` (the water draws and the post table's batches), instances, `CutSegment` (the post table and
  the water stage's scopes): used by the post stages and the water stage, which stage 3 replaces.
- `CanRun` and `RequestJoin`: every anchor client outside the stages, the skeleton's per-anchor path, and the clients of absent
  stages.
- Hosted clears (`HostReady`, the CPU record, `host skipped`/`host missing`): the DCLF shadow-view and Z-prepass hosts and the
  reflection faces' anchor.
- D3D11 `Replay` and `RunCopy`'s anchor run: other frame kinds, absent frames, and the clears that keep tickets in world frames.
- Guest mode: every anchor's own epoch, unchanged.

### What stage 3 should know

- A stage commits with one copy from its staging run. Anything a commit stores per run must go through a `LatchBlock`, a `RunGate`
  or `RunSlots`: the commit moves those. A gate a client does not list is handled now; other per-run state keyed by `CurrentRun()`
  is not (ImageSpace's `CommitBatch` keeps sources per run index, and a stable run names the same run every frame, so its list
  grows).
- The water stage's interior fault is the open item that blocks merging deferred and water into one stage.
- The recipe check reads each epoch's built schedule. A post stage that owns its targets will show its reads and writes there.
- Recording a built graph before its attach (so a build and its recordings switch in together) was not done:
  `BuildEpochGraph` leaves the alias placement to the graph's first preparation on the host's thread, against the running graph's
  shared transients, and `RequestEpochRecording` records only for an attached instance. Both would need a pending instance in the
  host.

## Recipe plan, stage 3: deferred+water, and the RenderWorld scopes as CS passes (2026-11-15, `org-native/phases`)

Stage 3 of the recipe plan asked for four parts: repair the water stage, the RenderWorld scopes as CS-owned passes, the
end-of-frame chain as CS-owned passes in one post stage, and the deletions. Parts 1 and 2 are done. Parts 3 and 4 were not
started: the post chain is still the two table-drawn stages of stage 2 (see "What is left" below).

### Part 1: the water stage, and one deferred+water stage

**The darkness did not reproduce.** Dragonsreach with the water stage on, on the stage 2 build (`CS_ORG_SEGMENT_WORLD=1`):

| Run (measured, mean brightness of the shot) | Water stage on | Water stage off |
|---|---|---|
| `native-interior` (coc straight to Dragonsreach, two shots 300 frames apart) | 104.2 / 99.6, 104.1 / 99.2 | 104.0 / 98.9 |
| `native-validation` (Dragonsreach after the save and load) | 91.9 | (stage 2: 93) |

The shot with the stage on matches the guest shot by eye. The dark shots of stage 2 (40 to 63) were all in the validation tour,
which earlier stages also saw spread from 86 to 99 with no code change. It is the known exposure-adaptation variance (a separate,
later item), not lost lighting. The water stage is on in every native world frame now; `CS_ORG_SEGMENT_WORLD` is gone.

**Merged.** The deferred and water stages are one stage, `deferred+water` (`SegmentId::Deferred`, `Segment::DeclaredDeferred`):
nine steps from Deferred::StartDeferred's three hooks through the water stencil and mask, the scopes, the kMAIN copy, the water and
the water blend, committed at the point just before DCLF's forward epoch (`FlushRegions`). The census saw no GPU work between its
first tap and its commit. The recipe has eleven stages; the EngineDraws anchor's own epoch (blood splatter, precipitation, the menu
scenes) is listed with the forward stage.

**Held sources.** `CommitBatch` keeps the sources a run's latched draws read (`Pending::source`) in a `RunSlots`. A stage's commit
moves its staging run's list onto the run that submits it (`RunSlotsRegistry::MoveAll`); the list is released when that run's lease
index runs again. The per-lease vector it replaces only grew for a staging run.

### Part 2: the RenderWorld scopes as CS passes

`src/RenderGraph/Engine/ImageSpace/WorldSteps.inl` (included by ImageSpace.cpp). Native mode with the deferred+water stage.

**The helper: an engine effect step as a CS pass.** `StepDesc` names the effect (CS's port of its shader and the effect's defines),
its output or outputs (an engine target, or one CS owns), its inputs by slot with their samplers (the engine's mode numbers:
`StepSampler`), its blend state (by the engine's four blend fields) and its viewport rule (`StepArea::Full`, or `Render`: the output
scaled by the dynamic-resolution ratios at the build). `MakeStepSpec` resolves it into a `Spec` sized for the targets now, and the
existing `MakePass` makes the ORG pass (program, bindings, samplers, gate, latch). The pass reads the engine's PerGeometry and
PerFrame blocks (a dispatch: its CS b0 block) by the heap index and offset its latch region holds; the step's data-only hook writes
them there. Nothing is read from the engine's device state: no `ReadShape`, no shape manifest, no table row.

**The data-only hook** (`TakeWorldStep`). At the engine's draw or dispatch of a step, the bound constants are captured
(`Constants::Capture`) and staged for the step's pass, and the engine's draw is withheld. The step is found by its scope (set by the
RenderSSAO, reflections, snow and water-blend hooks: `WorldScopeGuard`), its effect, and the variant the engine picked:

| Variant | How the hook knows it |
|---|---|
| the normals target (kNORMAL_TAAMASK_SSRMASK or its swap) | GetImageSpaceRenderTarget_TAAMASK_SSRMASK_SWAP's rule (100460): the engine frame counter's parity (525008 / 411489) with SSR on |
| the water blend's history | the frame counter's parity, as the engine picks it |
| the raw AO's history | CS's own pair: the frame slot's parity |
| the minify level | the minify loop's iteration in RenderSSAO (levels 1 to count - 1, once each, in order) |
| the composite's form | its effect (SAO, SAO and fog, fog), and the copy (ISCopy) for neither |
| the ray march's source | the SSR downsample switch (513568 / 391448) and mode (513454 / 391287) |

The scope's end runs the ImageSpace anchor once (`Flush`), which is the step's tap: the clients' commits write the staged entries
and open their gates in the stage's staging run. A step CS has no pass for at all is a `no CS pass` tap error (a mismatch: fatal in
development builds). One the frame's schedule has off draws nothing that frame, is counted as lagged work, and stays on in every
later description (`requestedScopes`).

**The passes**, in Main::RenderWorld's order, 45 scope passes (with 11 SAO levels) and 2 water-blend passes at 1920x1080:

| Steps | Effect | Reads | Writes | Constants | Reached |
|---|---|---|---|---|---|
| SSR mask downsample, by normals | ISDownsampleIgnoreBrightest | the normals | CS: `ImageSpace::SSRMaskDownsampled` (transient) | captured | yes |
| SSR ray march: from the mask, or from the normals by parity | ISReflectionsRayTracing | mask or normals, kMAIN, main depth, kMAIN_ONLY_ALPHA, reflection cube | kSSR_RAW | captured | yes (from the mask) |
| SSR blur H, V | ISReflectionBlurHCS, VCS | kSSR_RAW, kSSR_BLURRED0 | kSSR_BLURRED0, kSSR | captured CS b0 | yes |
| SAO camera Z | ISSAOCameraZ | depth target 7 | kSAO_CAMERAZ mip 0 | captured | yes |
| SAO minify, levels 1 to count - 1, and the contrast form at its level | ISMinify, ISMinifyContrast | the level above | kSAO_CAMERAZ mip k | captured | yes |
| SAO raw AO, temporal or not, by history and normals | ISSAORawAO(NoTemporal) | camera Z, normals, CS's other history (temporal), motion | CS: `ImageSpace::SAORawAO0/1` (history) | captured | temporal yes |
| SAO blur H by history, V | ISSAOBlurH, V | CS history, CS scratch | CS: `ImageSpace::SAOBlurTemp` (transient), `ImageSpace::SAO` | captured | yes |
| snow SSS | ISSnowSSS | camera Z mip 0, kMAIN, kSNOW_SPECALPHA (RenderSnowSSS, SE 0x1412ee8d0) | kSNOW_SWAP | captured | no (no snow scene) |
| snow copy | ISCopySubRegionCS | kMAIN | kSNOW_SWAP | captured CS b0 | yes |
| SAO composite, three forms, blurred or raw reflections, by normals | ISSAOCompositeSAO, SAOFog, Fog | kSNOW_SWAP, CS's blurred AO, depth 7, normals, kSNOW_SPECALPHA, shadow mask, kSSR or kSSR_RAW, CS's feature headers | kMAIN | captured | fog and SAO+fog yes |
| SAO composite copy (neither SAO nor fog) | ISCopy | kSNOW_SWAP | kMAIN | none | no |
| water blend, by history | ISWaterBlend | kRAW_WATER, the other kWATER, motion, main depth, the normals of that parity | kMAIN, kWATER_1 or kWATER_2 | captured | yes (river) |

Samplers are those RenderSSAO and the effects set (SE 0x1412ebd70: minify point to level 4 and linear after, the raw AO trilinear
then linear). Where the engine left a sampler unset (the water blend's third and fifth, the composite's first), the pass declares
clamp; the row values those took from earlier draws are gone with the table.

**Targets.** CS owns the SSR mask downsample (transient), the AO blur scratch (transient), the blurred AO (kept across frames: the
fog-only composite reads it in frames whose generation drew nothing) and the raw AO history pair, cleared once by one-shot UAV clears.
They replace kNORMAL_TAAMASK_SSRMASK_DOWNSAMPLED, kSAO_TEMP_BLUR, kSAO, kSAO_RAWAO and kSAO_RAWAO_PREVIOUS in native. Kept as engine
targets, for readers outside the scopes: kSAO_CAMERAZ (Upscaling's depth upscale writes it), kSSR, kSSR_RAW and kSSR_BLURRED0 (the
water surface reads them), kSNOW_SWAP and kSNOW_SPECALPHA (the snow targets Hooks.cpp makes), kWATER_1/2 and kRAW_WATER (the
skeleton's clears, the water's draws), kMAIN.

**Description.** The clients' rows are read at Begin (`WorldScopeRows`): SSR's passes when the reflections helper is active, the snow
pass or its copy by the snow flag, the composite form by the SAO and fog switches (the portal graph), the history passes of the
frame's parity; both normals variants are on (the hook opens the one the engine drew). The level count and the contrast level are
fixed at the build for kMAIN's size; a new size, level count or dynamic-resolution ratio registers the clients again at the next
world frame (an off-thread build; frames keep the old schedule). Registrations seen: one at the main menu, one per render ratio
(DLSS, FSR, XeSS at 0.667; TAA at 1.0).

**Deleted in native.** The scope rows of the pass-state table (32 rows), PostChain's scope prediction, check and per-slot world
fields (`PredictScope`, `ScopeStart`, `ScopeEnd`, `CurrentScope`, `SetScopeHook`, `DeclaredWorld`, `WorldSlotFields`), ImageSpace's
`OnWorldScope`, `RowDrawable`, the scopes' cut, and the water blend's shadow-state sampler read.

### Measurements

perf-taa, native Vulkan, Perf-Fast, Tracy, one run each, measured. Before: stage 2's final run (water stage off). After: this stage.

| perf-taa | Before (stage 2) | After |
|---|---|---|
| fps between the marks | 120.9 | 126.0 |
| Tickets a frame | 28.93 | 18.95 |
| `vkQueueSubmit` calls a frame (RHI; plus 1 external) | 34.80 | 29.14 |
| Entry / body barriers a frame | 899 / 1205 | 617 / 857 |
| Render thread busy, ms a frame | 3.59 | 3.17 |
| Submission thread busy, ms a frame | 3.69 | 2.94 |
| Host thread busy, ms a frame | 2.34 | 1.72 |
| Recordings, switches, cuts and tap errors in the steady window | 0, 0, 0, 0 | 0, 0, 0, 0 |

The deferred+water ticket against what it replaces (measured):

| Per frame | deferred+water (after) | Before: deferred stage + 5 ImageSpace + 2 EngineDraws + EngineCopy + 2 clear tickets |
|---|---|---|
| Tickets | 1 | 11 |
| Submission thread `FinishTicket`, us | 351 (admission 14, inline recording 200, `vkQueueSubmit` 61) | 109 for the deferred stage, plus about 600 for the others (stage 0's figures: 2 x 145, 5 x 54, 45) |
| Render thread `ExecuteEpoch`, us | 96 | 13 for the deferred stage, plus about 170 for the others |
| Entry / body barriers | 76 / 164 | 36 / 78 for the world segment of stage 0, plus the deferred stage's |

The 200 us of inline recording is the stage's late batch: the water draws, the deferred composite and the kMAIN copy are recorded at
execution (their passes are not GPU-gated). The world steps are GPU-gated and recorded ahead.

### Checks

One run each, `CS_TEST_BACKGROUND=1`, mismatches and recipe findings fatal: native-quick (DLSS), native-d3d12-quick, native-river,
native-upscalers (DLSS, FSR, TAA at 1920x1080 render, XeSS), native-validation (the known 20x `VUID-vkCmdDraw-None-09600` only;
Dragonsreach 88.8, the same image as part 1's 98.1 with a whole-image exposure offset; inventory and map as before), the loading-thread
run, first and third person, guest-quick: all passed, 0 tap errors, 0 recipe findings. Shots against the part 1 build: the same by
eye; differences are foliage and character motion, and TAA's 4.0 mean difference is inside stage 2's own run-to-run 5.5. The fatal
path did not fire. ORG was not changed.

### What is left

- **Part 3, the post stage:** done; see "The post stage" below. Formerly: The end-of-frame chain is still drawn from the pass-state table in two stages around the
  upscaler's evaluate, with its cuts and decision events. The helper above is what it needs: each chain step as a `StepDesc`, with a
  data-only hook per effect step and CS-owned targets and histories. The HDR effect's luminance targets (kHDR_DOWNSAMPLE4/6) stay
  engine targets for DCLF's character light; the upscaler evaluate as a pass needs each SDK recording into the stage's list.
- **Part 4, the deletions**, follows part 3: the table and its generator, PostChain's prediction, `CutSegment`, shared and point
  steps, `repeats`, the shape manifest's use.
- Still reachable in native from ImageSpace.cpp: the post table clients and their cut path (the chain), the main client with the
  shape manifest and late clients (flow copies, water displacement, menus, the chain's cut parts), and other frame kinds.
- Constants are captured, not computed. RenderSSAO's constants are all computed in its body from globals (SE 0x1412ebd70: the
  camera's planes, the level sizes, the SAO settings at 0x143233548 to 0x14323356c): computing them at Begin would drop the SAO hooks'
  captures. Not done.

## The post stage (recipe plan, stage 3, part 3; 2026-11-15, `org-native/phases`)

The world post chain (RenderWorldPostProcess, SE 100430 / AE 107148) is drawn by passes CS declares. The pass-state table, its
generator and check, the post description's prediction and decision events, and `CutSegment` are deleted. Native mode; guest keeps
its per-anchor path.

### How it works

`src/RenderGraph/Engine/ImageSpace/ChainSteps.inl`, on the step helpers of `WorldSteps.inl` (`StepDesc`, `MakeStepSpec`, `StepKey`,
`MakeOwned`, and `StageStep`, the data-only part of a hook, now shared by both files).

- **One stage.** The post stage (`SegmentId::Post`, `Segment::DeclaredPost`) is one revision-driven epoch and one ticket: the
  volumetric lighting, the composite, the PostProcessing anchor, the Upscale anchor (the upscaler's evaluate, the copy into kMAIN,
  the depth upscale), the slots and the HDR effect, the Tonemap anchor, the tonemap and after, and RenderEndOfFrameEffects' return
  as its end point. The post composites and post end-of-frame stages are gone.
- **Passes.** A fixed list in the engine's order, sized at the build. Three clients share the stage's rows in order: the composite
  (first ImageSpace step), the slots before the tonemap and the HDR effect up to its bloom (second, after the Upscale anchor), the
  tonemap and after (third, after the Tonemap anchor). 2 + 92 + 101 passes at 1920x1080 with 4 luminance levels.
- **Hooks.** Each draw or dispatch inside the world chain stages the constants the engine bound and is withheld
  (`TakeChainStep`). Its pass is found by scope, effect and variant (`ChainKeyOf`).
- **Targets are fixed** (since the fixed-targets pass below; before it, `PlanEndOfFrame` replayed the engine's loop at
  RenderEndOfFrameEffects' entry and picked one of several in/out variants per step). Every step reads and writes the same targets
  whichever other steps are present or run: the chain works in place on kMAIN before the tonemap and on the final target after it,
  and an effect that cannot write the image it reads draws into its side's scratch target and a copy puts the result back
  ("Fixed targets", below).
- **Description at Begin** (`ChainRows`, from `Description::post`): the composite with the volumetric lighting, each slot in its
  place (usesLDR), the HDR effect or its copy, the last luminance level's form (adaptation or not), the bloom of the frame's radius,
  the tonemap or its fade form (off when Effects11 owns it), TAA from CS's upscaler method (`UpscaleMethod::kTAA`; without
  Upscaling, the engine flag), the upsample from dynamic resolution, the screenshot copy always. Variants picked per frame
  (parities) are all on. TAA's history is CS's pair by the frame slot's parity, so a slot records only its own history's passes.
- **Lag, not cuts.** A hook whose group the schedule has off draws nothing, counts lagged work, and turns the group on in later
  descriptions (`requested`). The image stays as the step before it left it. A draw CS has no pass for is `NoPass` (fatal in
  development builds). Within 300 frames of a registration (a new size), an unknown variant counts as lagged work instead.
- **Resizes.** kMAIN's size, the final target's size (from the render target manager: the renderer's own entry has no texture in
  native mode), the luminance level count and the dynamic-resolution ratios are checked at each world frame; a change registers the
  clients again (off-thread build).

### The steps

| Step | Effect(s) | Reads | Writes | Variant chosen by | Reached |
|---|---|---|---|---|---|
| composite (VL form) | ISCompositeVolumetricLighting, blend mode 5, render-size viewport | kIMAGESPACE_VOLUMETRIC_LIGHTING + parity | kMAIN | VL state counter (100297 / 107014, +0x88) at the hook | yes |
| world map (slot 0) | ISWorldMap, NoSkyBlur | kMAIN, main depth | HdrScratch, then its copy back into kMAIN | its form | yes (validation map) |
| refraction (slot 1) | ISRefraction | kMAIN, kREFRACTION_NORMALS | HdrScratch; copy back | - | yes |
| FXAA (slot 2) | ISFXAA | kMAIN | HdrScratch; copy back | - | no (unverified) |
| depth of field (slot 3, both places) | ISHDRDownSample4, ISDepthOfFieldBlurH/VCS, ISDepthOfField(Fogged), ISDistantBlur(Fogged) | the image (kMAIN, or the final target after the tonemap), CS downsample, main depth, kSAO_CAMERAZ mip 10 | CS DofDownsample, DofBlurSwap; the side's scratch; copy back | composite by its effect (the step flags) | after the tonemap yes (inventory, perf-taa); before it no |
| radial blur (slot 5, both places) | ISRadialBlur, Medium, High | the image | the side's scratch; copy back | its form | yes (validation map) |
| HDR copy | ISCopy | kMAIN | the final target | (HDR switch 513463 / 391301 off) | no |
| HDR downsample | ISHDRDownSample4 | kMAIN | kHDR_DOWNSAMPLE0 | - | yes |
| luminance levels | ISHDRDownSample4RGB2Lum, 16Lum | the level above | kHDR_DOWNSAMPLE1.. | level index (count fixed at build) | yes |
| last level | 16Lum or ISHDRDownSample16LightAdapt | the level above (and the other parity's target) | 27 + levels - 1 + 2 x parity | the parity global 528109 / 415054 at the hook (flipped by the engine before the draw) | yes |
| bloom | ISBrightPassBlur(r), ISBlur(r) | kHDR_DOWNSAMPLE0 and the last level; CS scratch | CS BloomSwap, CS Bloom | radius at Begin (+0xcc); parity | radius 7 yes |
| tonemap | ISHDRTonemapBlendCinematic(Fade) | CS Bloom, kMAIN, last level | the final target | parity, fade (+0x118) | plain yes, fade no |
| TAA | ISTemporalAA | the final target, CS history (other), motion, depth, TAA mask, normals | LdrScratch and CS history; copy back into the final target | frame slot parity, Deferred's normals target | yes (upscalers TAA, perf-taa) |
| screenshot | ISCopy | the final target (after TAA, before the LDR slots) | kSAVE_GAME_SCREENSHOT | - (+0x211 opens it) | no |
| upsample | ISUpsampleDynamicResolution | the final target | LdrScratch; copy back | - | no |
| full-screen blur (slot 6, both places) | ISHDRDownSample4, ISBlurN (or ISNonHDRBlurN by the blur effect's HDR flag), ISHDRDownSample4 | the image; CS FullBlurDown, FullBlurSwap | CS FullBlurDown, FullBlurSwap; the image itself (in place, blended below radius 1). The engine's copy of its input into its output (ISCopy or ISCopyDynamicFetchDisabled) is the image itself: its draw has no pass | the effect's draws in order (InnerVariant: shader, its occurrence, the slot), radius at the draw (+0x128) | no |
| get hit (slot 7, both places) | ISCopy, ISBlur5 four times, ISDoubleVision | the image; CS GetHitA, GetHitB, GetHitSwap | CS buffers; the side's scratch; copy back | the same | no |

Constants: every step's are captured (data-only hooks). None is computed yet.

The full-screen blur and get-hit effects (ImageSpaceEffectFullScreenBlur, Setup SE 0x141329df0, Render 0x14132a370;
ImageSpaceEffectGetHit, Setup 0x141328b80) are composite effects: each Render draws a fixed chain of the manager's shader effects,
wired by the effect's step table (shaders at +0x18, textures at +0x48, inputs at +0x60, outputs at +0x78). Their Render vfuncs are
wrapped (at ImageSpace's Setup, after Legacy Graphics Compatibility patched the full-screen blur's) so a draw inside them is keyed by
its shader and how many draws of that shader came before it in the effect (`InnerVariant`). The borrowed textures (kGETHIT_BUFFER
twice, kGETHIT_BLURSWAP; kHDR_BLURSWAP, kHDR_DOWNSAMPLE0) are CS transients made like those targets.

Not built (a hook for them is `NoPass`):
- the lens flare forms of the composite and the image-based lens flares: ISCompositeLensFlare reads the output of the image-based
  lens flare chain (manager +0x180), several passes over kIBLENSFLARES targets whose order and inputs are not decompiled;
- SIL (ISSILRawInd, ISSILComposite, manager +0x1d8): off in the game's settings, and not decompiled;
- slot 4: the effect list has no entry 4 on SE (CommonLib: "Missing SE 4"), so nothing draws there;
- the depth of field's raster blur (the compute-blur option off) and its masked composites: the masked forms read a mask target
  whose producer is not decompiled; the raster blur is an ImageSpaceEffectBlur chain like the full-screen blur's and could follow it;
- TAA's UI and water forms (ISTemporalAA_UI, ISTemporalAA_Water, SE 0x1412eeb90): CS has no port of either shader, and the water
  form writes a third target (kTEMPORAL_AA_WATER_1/2 by history) that StepDesc cannot express. The UI form (helper +0x2c = 2) uses its
  own history pair (kTEMPORAL_AA_UI_ACCUMULATION_1/2).
No test reaches any of these, nor the full-screen blur or get hit.

### Targets

- CS-owned: `ImageSpace::Bloom`, `BloomSwap` (in place of kHDR_BLOOM, kHDR_BLURSWAP), `DofDownsample`, `DofBlurSwap` (kLDR_DOWNSAMPLE0,
  kLDR_BLURSWAP), `GetHitA`, `GetHitB`, `GetHitSwap`, `FullBlurDown`, `FullBlurSwap`, transients; `TAAHistory0/1`
  (kTEMPORAL_AA_ACCUMULATION_1/2) and `AdaptedLuminance0/1` (the luminance chain's last level of each parity, 27 + levels - 1 + 2 x
  parity), history pairs cleared once.
- The adapted luminance pair is published (`ImageSpace::AdaptedLuminance0/1`). Drawcall Limit Fix's character light, which reads the
  engine's adapted luminance target the engine binds, reads CS's target of the same parity instead (`AdaptedLuminanceTextureIndex`,
  `ImageSpace::AdaptedLuminanceOf`); its passes declare both ids.
- `HdrScratch` and `LdrScratch` (transients, made with kMAIN's and the final target's size and view format): an effect draws there
  and its copy back moves the result into its image ("Fixed targets", below).
- Engine targets kept, each with the reader that needs it: kMAIN (the image before the tonemap: the Upscale anchor writes it, the VL
  composite blends into it, Post Processing writes its result back into it and Effects11 its corrected image); the final target (the
  image after the tonemap: the UI stage draws over it, frame generation's hudless copy, HDR output and the present read it);
  kHDR_DOWNSAMPLE0 and the levels before the last (the character light may still bind one of them; nothing else outside the chain
  reads them); kSAVE_GAME_SCREENSHOT (the save's thumbnail); the VL pair (Volumetric Lighting's port writes it); kSAO_CAMERAZ mip 10
  (the depth of field's average depth; the depth upscale writes it); kMOTION_VECTOR, the main depth, kTEMPORAL_AA_MASK and the normals
  targets (TAA and the upscaler read them, earlier stages write them); kREFRACTION_NORMALS (the forward pass writes it).
- No longer used by the post stage: kMAIN_COPY (the engine's swap target before the tonemap; the water surface and refraction still
  read it, written outside the post stage), kIMAGESPACE_TEMP_COPY and kIMAGESPACE_TEMP_COPY2.

### Measurements

perf-taa, native Vulkan, Perf-Fast, Tracy, one run each, measured. Before: stage 3 part 2's final run (`s3w1perf`).

| perf-taa | Before | After |
|---|---|---|
| fps between the marks | 126.0 | 123.0 |
| Tickets a frame | 18.95 | 18.91 |
| `vkQueueSubmit` calls a frame (RHI; plus 1 external) | 29.14 | 29.00 |
| Entry / body barriers a frame | 617 / 857 | 612 / 883 |
| post end-of-frame stage: entry / body barriers | 32 / 92 | 28 / 120 |
| Render / submission / host thread busy, ms a frame | 3.17 / 2.94 / 1.72 | 3.01 / 2.91 / 1.68 |
| Post composites ticket: submission us (admission, inline, `vkQueueSubmit`); render us | 20.5 (5.6, 1.2, 1.1); 17.5 | 18.5 (4.7, 1.0, 1.7); 13.8 |
| Post end-of-frame ticket: submission us (admission, inline, `vkQueueSubmit`); render us | 54.8 (8.5, 0, 28.7); 16.7 | 49.9 (9.8, 0, 24.4); 17.4 |
| Recordings, switches, lagged work, tap errors in the steady window | 0, 0, 0, 0 | 0, 0, 0, 0 |

The frame rate difference is inside run-to-run spread (earlier pairs of the same build differed by 2 to 3 fps). The body barriers of
the end-of-frame stage rise by 28: both last-level forms' targets and the TAA variants are in the graph. The stage count is
unchanged: the upscaler's evaluate is still external work between the two post stages (below).

### Checks

One run each, `CS_TEST_BACKGROUND=1`, mismatches and recipe findings fatal: native-quick (DLSS), native-d3d12-quick, native-river,
native-upscalers (DLSS, FSR, TAA, XeSS registered; switches at run time), native-interior, native-validation (Whiterun, inventory, map,
save and load, Dragonsreach; the known 20x `VUID-vkCmdDraw-None-09600` and the BackBuffer layout warning only), the loading-thread
run, first and third person, guest-quick: 0 tap errors, 0 recipe findings, no `NoPass`. The fatal path fired once in development (a
slot the plan had left out was looked up as a variant); fixed. Shots match the previous build by eye; the map's world map and
radial blur, which used to cut, are drawn. Adaptation: native-interior's two Dragonsreach shots, 300 frames apart, read 100.6 and
96.5 against 99.8 and 95.3 (and 99.7 to 104 over other earlier runs): the same exposure and the same drift between the shots. The
recipe shows each adaptation pass reading kHDR_DOWNSAMPLE3 and the other parity's target, as the decompile does.

### The upscaler's evaluate as a pass (stage 3 finish)

In native mode the evaluate is the Upscale anchor's first pass (`EvaluatePass`, ORGUpscaling.cpp), in the shape of BasicRenderer's
`UpscalingPass`:
- `cs_EvaluateFeatureNative` sets the frame's constants (`slSetConstants`) on the render thread, then hands the tags and the
  evaluate to `ORGUpscaling::QueueEvaluate` as a closure that captures the frame token, viewport, feature and extents by value. The
  Upscale client's commit stores it for the run (`RunSlots`), so the stage's commit moves it with the other staged inputs.
- The pass is recorded at execution each frame (the stage's late batch, on the submission thread): `SkipsAtExecution` when the run has
  no evaluate (TAA, no upscaler), else `Record` calls `slSetTagForFrame` and `slEvaluateFeature` on the pass's own command list, then
  `RestoreDescriptorHeaps`. A kept recording cannot hold it: the token and the SDK's commands are new each frame.
- Its four images (kMAIN in, the upscaled texture out, kMAIN's depth, kMOTION_VECTOR) are legacy interop uses
  (`PassBuilder::LegacyInteropUse`, new in ORG: a common-state use with a token). The common state is GENERAL on Vulkan and COMMON on
  D3D12, which is what the tags name. On D3D12 the output also declares an internal transition to the common state, as BasicRenderer
  does. `Native::DescribeForCommands` turns the pass's list and resolved images into the native list, images, Vulkan views (retained
  by the recording) and states.
- `Native::RunExternal` and `ExternalImage` are deleted; nothing else used them.

The post composites and post end-of-frame stages are merged into one post stage (above). What runs between the composite and
RenderEndOfFrameEffects (Post Processing's `DrawBeforeUpscaling`, the frame generation border clear, the depth upscale) submits no
GPU work of its own in a steady frame (the census line below).

Two FrameAnchors rules came with the merge, both for frames where a stage's commit point did not come (Drawcall Limit Fix's forward
epoch did not run while its programs compile, seen on D3D12 at the world's first frames): a stage's first tap ends the earlier stages
that staged and are still open, at their end point (their tickets go first, in recipe order); and a tap is offered to every open stage
with a step at its anchor, not only the first (the deferred+water and post stages both have ImageSpace steps).

### The TAA mask and kMAIN_ONLY_ALPHA clears

The engine's clears of the TAA mask and kMAIN_ONLY_ALPHA after the water blend are clear passes of the deferred+water stage, first at
its last ImageSpace step (`Host::ImageSpace` in Skeleton.cpp): the engine's call stages their gate entries and colours, the stage's
commit carries them. The TAA mask's clear inside the post chain, when TAA is turned on, is a clear pass of the post stage, first at its
last ImageSpace step (`Host::PostChain`, the client `cs.engine.skeleton.frame@post-chain`). The frame group's clear ticket
(EngineFrameClear) is gone from steady world frames: one ticket fewer.

### Adaptation, read back

`CS_ORG_ADAPT_READBACK=1` copies, at the end of the post stage, the luminance chain's last input level and both adapted luminance
targets into a readback ring, and logs them five frames later every 30 frames (R11G11B10 decoded, red channel). native-interior
(Whiterun, then `coc WhiterunDragonsreach` at frame 917, shots at 1517 and 1817), measured, every other line:

| Frame | Last input level, mean | Adapted (parity 0 / 1), mean |
|---|---|---|
| 525 | 0.604 | 0.596 / 0 (not written yet) |
| 645 | 0.600 | 0.594 / 0.594 |
| 825 | 0.573 | 0.568 / 0.568 |
| 885 (loading) | 0.360 | 0.358 |
| 945 (Dragonsreach) | 0.453 | 0.451 |
| 1125 | 0.389 | 0.385 |
| 1365 | 0.373 | 0.369 |
| 1605 | 0.359 | 0.355 |
| 1785 | 0.349 | 0.347 |

The adapted luminance follows the scene's luminance within about 1 % a few frames after each change, in both parities, and the
character light reads the same targets. The scene's own luminance (the input, before any exposure) drifts down by about a quarter over
the 840 frames in Dragonsreach while nothing moves: the drift between the two Dragonsreach shots is in the lit scene, not in the
adaptation. That drift is the known brightness variance between runs (a separate item).

### Measurements (stage 3 finish)

perf-taa, native Vulkan, Perf-Fast, Tracy, one run each, measured. Before: the post stage's previous build (`s6perf`, this section).

| perf-taa | Before | After |
|---|---|---|
| fps between the marks | 123.0 | 121.1 |
| Tickets a frame | 18.91 | 16.96 |
| `vkQueueSubmit` calls a frame (RHI; plus 1 external) | 29.00 | 27.13 |
| Entry / body barriers a frame | 612 / 883 | 596 / 996 |
| post stage(s): entry / body barriers | 28 / 120 (end-of-frame) plus the composites stage's | 33 / 257 |
| Render / submission / host thread busy, ms a frame | 3.01 / 2.91 / 1.68 | 3.32 / 2.80 / 1.51 |
| Post stage ticket: submission us (admission, inline recording, `vkQueueSubmit`); render `ExecuteEpoch` us | 18.5 + 49.9; 13.8 + 17.4 (two stages) | 52.8 (12.8, 13.8, 0.9); 24.3 |
| Recordings, switches, lagged work, tap errors, recipe findings in the steady window | 0, 0, 0, 0, 0 | 0, 0, 0, 0, 0 |

Two tickets fewer: the post stages merged, and the frame group's clear ticket left. The body barriers rise by 113: the passes a
description has off still record their barriers, and the stage holds 66 more passes (the full-screen blur, get hit, the adapted pair's
clears). The frame rate is 1.9 fps below the last run, inside the 2 to 3 fps spread pairs of one build have shown; not confirmed
either way with one run each. In the steady window no GPU work is submitted between a stage's first tap and its commit; at the world's
first frames one ImageSpace anchor run and one native context operation are (both seen on the previous build too).

### Checks (stage 3 finish)

One run each, `CS_TEST_BACKGROUND=1`, mismatches and recipe findings fatal, on the final build: native-quick (DLSS registered, the
evaluate pass on Vulkan), native-d3d12-quick (the evaluate pass on D3D12), native-river, native-upscalers (DLSS, FSR, XeSS, TAA),
native-interior, native-validation (the known 20x `VUID-vkCmdDraw-None-09600` only), the loading-thread run, first and third person
(with the `start` shot), guest-quick: all passed, 0 tap errors, 0 recipe findings, no `NoPass`, no evaluate errors. Shots match the
previous build by eye: the differences are foliage in the wind, NPCs and the volumetric light's shafts in Dragonsreach (mean 101.8
against 99.4). The D3D12 run failed fatally twice during the work (the composite's tap offered to the deferred+water stage, which was
still open in a frame Drawcall Limit Fix did not draw); the two FrameAnchors rules above fixed it.

### What is left

- The lens flare forms, SIL, the depth of field's raster blur and masked composites, TAA's UI and water forms (The steps, above).
- Constants: none computed yet. RenderSSAO's (SE 0x1412ebd70) are computable from globals; the HDR effect's are a few reciprocal
  sizes plus globals (0x1412e6fe0).
- A description's off passes record their barriers; with many variants in one stage (the post stage's 195) that is a few hundred
  barriers a frame.
- Still reachable in native from ImageSpace.cpp: the main client, the shape manifest and late clients (flow copies, water
  displacement, menus and other frame kinds, UI3DSceneManager's end-of-frame call), until stages 4 and 5.

## Off passes cost nothing, and the start stage (recipe plan, stage 4; 2026-11-15, `org-native/phases`)

### Off passes: their barriers fold into the next pass that runs

Frame rate fell 126.0 -> 123.0 -> 121.1 over the post stage's two steps while body barriers rose 857 -> 883 -> 996. The cause: a
pass its description has off (`SetSkipWhen`: a row or client the description clears, an instance past the declared count) was
skipped ahead in the kept recording, but still recorded its before and after barriers, each its own barrier batch. The post stage
holds 195 passes, most off in any one frame (the full-screen blur, get hit, the variants of other descriptions).

Absent passes per description were considered first and not done here, because a build then dropped every slot's schedule until
its recordings were ready. Staged builds (stage 5) removed that reason: a build switches in whole with its recordings and never blanks
a stage. Off passes are now absent from the builds wherever a rebuild's latency is acceptable; see "Absent passes" below. Folding
stays for what remains present and off in a frame (parity variants, a lagging description).

What is done, in ORG (`FoldSkippedPassBarriers`, RhiExecution.h, applied by `BuildPersistentRecordingList`): when a list is built,
the barriers of each run of passes skipped ahead are folded into the next pass that runs.
- The transitions a run plans on one subresource compose into one: from the state before the run to the state after it. They go into
  the before batch of the next pass that runs, composed with that pass's own where they name the same subresource. A run at the end of
  the list goes into the last pass's after batch.
- A composed transition between equal states is dropped (the compiler's own test: same layout, and no access or sync change, or a
  UAV).
- Every backing ends in the state the plan gives it, so the ledger's commits and the next stage's entry barriers are unchanged.
- An aliased backing's first use (from Undefined) has sync All in its before state, so it still waits for the memory's earlier
  occupant when it shares a batch with other barriers.
- The list is left as it is when a skipped pass's barrier transfers queue or device ownership, or when two barriers of one batch would
  name overlapping but different ranges of a resource (barriers of one batch are unordered against each other).
- Only passes skipped ahead (known when the list is built) fold. A pass that skips at execution (a late batch's closed gate) still
  records its barriers: its answer changes per run, and a GPU-gated pass's barriers are needed whatever its count.

The census's body barriers are now counted from the built lists (`PersistentSegmentRun::bodyBarriers`), not from the plan.
`CS_ORG_KEEP_SKIPPED_BARRIERS=1` keeps the barriers where the plan put them (ORG's `DiagnosticKeepSkippedPassBarriers`), for A/B runs.

perf-taa, native Vulkan, Perf-Fast, Tracy, one run each, measured:

| perf-taa | Stage 3 finish (`s6perf` build's successor) | Folding only | Final build, folding on | Final build, `CS_ORG_KEEP_SKIPPED_BARRIERS=1` |
|---|---|---|---|---|
| fps between the marks | 121.1 | 124.1 | 123.3 (121.6 in an earlier run of a build one fix older) | 122.5 |
| Body barriers a frame | 996 | 843 | 811 | 964 |
| post stage body barriers | 257 | 104 | 104 | 257 |
| Render thread busy, ms a frame | 3.32 | 2.95 | 3.04 | 2.92 |

The barriers fall by 153 a frame, all in the post stage. The frame rate does not move outside the run-to-run spread: on one build,
folding on against off is 123.3 against 122.5, and runs of one build have spread 2 to 3 fps (121.6 to 124.1 here). So the fall from
126.0 to 121.1 was mostly that spread, not the barriers. The render thread's breakdown is the same in every run (Drawcall Limit Fix's
claims 0.52 to 0.57 ms, the present 0.38 to 0.41, the reflections prepass 0.30 to 0.35, the frame values kick 0.28 to 0.33): it does not
bound the frame (3 ms of 8), which is GPU-bound.

### The start stage

`SegmentId::Start`, `Segment::DeclaredStart` (FrameAnchors.cpp, kDeclaredInfo). Native mode with stages on; `CS_ORG_SEGMENT_START=0`
turns it off (its anchors keep their tickets and the faces' clears their own anchor's ticket). Guest mode keeps its per-anchor path.

The frame sequence census of a world frame (native-quick, before this stage):

```
build point (Renderer::Begin)
run ReflectionsPrepass [cs.cloud-shadows]                                                   -> its ticket
run SkyReflections [cs.engine.skeleton.cube@sky-reflections, cs.cloud-shadows.layers, cs.sky.faces] -> its ticket   (face 1)
run SkyReflections [...]                                                                     -> its ticket   (face 2)
submit CS DCLF: shadow view
```

The stage holds both anchors wholly (they have no epoch of their own): the ReflectionsPrepass step (Cloud Shadows' occlusion copy and
layer operations, IBL and Terrain Shadows' reflection prepasses where they have passes), the SkyReflections step, and a point end. It
is one graph, one kept recording per frame slot, one ticket.

**Commit point.** The point just before the next GPU work that follows the stage in every world frame is submitted (FlushRegions):
DCLF's shadow view, the sky occlusion, the shadow clears' anchor, the Z-prepass, or a later stage. A later stage's first tap also ends
it (the rule stage 3 added). A stage that ends at a point and staged nothing this frame ends there without a ticket, and does not
hold a later stage's window shut (`PointEnded`, WindowOpen).

**Reflection faces as fixed instances.** How many faces the engine draws a frame, and which, is its late decision. The faces' sky
client (`cs.sky.faces`) has six instances (one per face a frame may draw), all declared every frame: the description never changes
with the engine's choice. The k-th SkyReflections tap of the frame is instance k: its commit writes instance k's gate entries, latch
regions and run (`InstanceEntry`, `RunSlots` per instance). An instance the engine does not draw has its entries closed and records
nothing. The face's draw list is a frame input: the pass is in the stage's late batch, recorded at each submission from the run its
instance staged (`SkyPass` records late in a CS stage: `RecordsLate`), so the kept recording never changes with the face or the
weather.

**Clears as load operations.** The face sky's first draw of each instance clears the face (to zero; the sky draws its background)
and the faces' depth (1, stencil 0, the engine's values) with its render pass's load operations (`RasterProgram::Depth::clearStencil`,
new). The engine's clears of the faces and of kCUBEMAP_REFLECTIONS in a world frame are dropped (the engine skeleton's
`FacesClearedBySky`), and the hosted face clears (`Host::SkyReflections`, its CPU record and its client at the anchor) are deleted.

**Cloud Shadows' layer operations** run at both steps. At SkyReflections each face's tap writes its own entry set and latch rows
(one set per face, `LayerOps::instances`), and takes scratch slots after those the frame's earlier face taps took, since every set's
gathers run before the scatters. The client `repeats`.

**What stays a ticket at the frame's start, and why.**
- The decal occlusion tests (DecalQueries): several a frame, one per decal in the test state, each with its own query range of the
  run (QueryRing per run). Instances would need a query range per instance; not done. They run before the stage's first tap.
- The water's displacement update and flow copies (the main ImageSpace client, `cs.engine.imagespace`, built from the shape manifest):
  in the river they run after the last face's sky, inside the stage's window, on the ImageSpace anchor's own ticket (0.47 a frame), and
  run ahead of the stage's work (independent targets). Taking them into the stage needs their effects as CS steps (StepDesc, a
  data-only hook each, the ripple and flow lists as frame inputs): the main client's commit reuses its gate entries per run, so it
  cannot be a step that repeats. Not done.

### The main pass's sky in pre-main

The pre-main stage gains the Sky step (the main pass's sky batch, after the Prepass clients, before DCLF's main opaque epoch) and ends
at a point: just before DCLF's main opaque epoch, the deferred+water stage or the forward epoch is submitted, or at the next stage's
first tap. The sky's draw list is a frame input recorded in the stage's late batch. The sky anchor's ticket is gone.

### The recipe

Start: the decal queries' anchor, the start stage, the cube clear anchor (outside world frames). Depth gains Drawcall Limit Fix's water
reflection faces (after the Z-prepass, where the epoch runs; the recipe had it in the start stage). Opaque is DCLF's main opaque alone.
`kPreviousFrame` loses `Engine::kMAIN_ONLY_ALPHA`: since stage 3 its clear is a pass of the deferred+water stage, the stage whose ray
march reads it, so the read is the stage's own (a read is fine when the reading stage writes the resource).

perf-taa, native Vulkan, Perf-Fast, Tracy, one run each, measured. Before: stage 3 finish (`s6perf`'s successor, the doc's last table).

| perf-taa | Before | After (final build) |
|---|---|---|
| fps between the marks | 121.1 | 123.3 |
| Tickets a frame | 16.96 | 13.96 |
| `vkQueueSubmit` calls a frame (RHI; plus 1 external) | 27.13 | 24.06 |
| Entry / body barriers a frame | 596 / 996 | 506 / 811 |
| Render / submission / host thread busy, ms a frame | 3.32 / 2.80 / 1.51 | 3.04 / 2.26 / 1.17 |
| Recordings, switches, tap errors, recipe findings in the steady window | 0, 0, 0, 0 | 0, 0, 0, 0 |

| Per frame (after, measured) | Tickets | Entry / body barriers | Render `ExecuteEpoch`, us |
|---|---|---|---|
| start stage | 1 | 8 / 137 | 41.9 |
| it replaces: reflections prepass + 2 reflection sky (before) | 3 | 90 / 174 | 20.8 + 2 x 22.2 |
| pre-main with the sky | 1 | 67 / 33 | 31.7 |
| it replaces: pre-main + sky (before) | 2 | 75 / 27 | 26.8 + 14.9 |

The entry barriers fall by 90: the faces' targets enter once a frame instead of once per face, and the face clears are load
operations. The steady world frame's tickets: start, shadow view, pre-depth, Z-prepass, pre-main, main opaque, deferred+water,
forward, the sun's queries (1.96), first person, post, interface, overlay.

### Checks

One run each, `CS_TEST_BACKGROUND=1`, mismatches and recipe findings fatal: native-quick (DLSS registered; with the frame sequence
census), native-d3d12-quick, native-river (sequence census), native-upscalers (DLSS, FSR, TAA, XeSS), native-interior,
native-validation (the known 20x `VUID-vkCmdDraw-None-09600` only), the loading-thread run, first and third person (with the `start`
shot), guest-quick: all passed, 0 tap errors, 0 recipe findings. Shots match the previous build's by eye (native-quick 105.6 against
105.5; river 76.6 / 77.4 against 77.0 / 77.0). The steady recipe: 484 passes, 8 inputs from the previous frame, 109 from outside the
graphs, 0 findings. In the steady window: 0 recordings, 0 schedule switches, nothing submitted between a stage's first tap and its
commit (in the river, the water effects' ImageSpace ticket is, see above).

The fatal paths fired three times during the work, each fixed before the final runs:
- `host skipped`: the engine's shadow clears are handed to DCLF's shadow view, and the start stage committed just before it. The skeleton
  no longer counts the start stage's submission against waiting hosted work (its work is earlier in the frame and touches no hosted
  target).
- `missing` (pre-main's end, D3D12 and the validation tour at the world's first frames): DCLF's main opaque did not run and the deferred
  stage was absent, so nothing ended pre-main. An absent stage's first tap now ends the earlier stages too, and work that ends the world
  segment (forward, the interface, the present) ends the start and pre-main stages.
- The cs-test plugin refused `CS_ORG_KEEP_SKIPPED_BARRIERS` until it was in the switch registry.

### What is still a ticket at a hook in a world frame, and why

- The sun's occlusion queries (EngineQueries, two a frame: after DCLF's forward epoch and after first person). Each is between two DCLF
  epochs: taking them into a stage would move the first after the first-person epoch on the GPU, where the first-person geometry would
  occlude the sun. They need a stage between forward and first person, or the queries as DCLF passes.
- The decal tests and the water effects at the frame's start (above).
- The interface and the overlay (the UI stage, stage 5).
- Outside steady frames: the engine's clears that keep their own anchor (the precipitation occlusion map, the cube faces outside world
  frames), the EngineDraws anchor's own clients (blood, precipitation, menu scenes), the ImageSpace main client.

### What stage 5 should know

- A stage that ends at a point commits just before the next GPU work that always follows it (FlushRegions' Ends* lists), and at a later
  stage's first tap, present or absent. One that staged nothing ends without a ticket and does not hold later windows shut. The UI
  stage can end at the present the same way.
- A hook that repeats inside a stage with a late engine decision (faces, Scaleform displays, Bink tiles) takes fixed instances: declare
  them all every frame, give each its own gate entries, latch regions and `RunSlots`, and let the ones the engine does not use stay
  closed. Draw lists are frame inputs recorded in the late batch (pass constructed with "records late" in a CS stage).
- A client at a repeated step that cannot have per-instance passes can `repeat` with an entry set per tap and one pass dispatching
  every set (Cloud Shadows' layer operations); shared scratch must be split between the taps.
- Clears as load operations need the clear to come with the first draw of the pass that writes the target; the engine's clear call is
  then dropped in the frame kinds that have the stage (the skeleton's `FacesClearedBySky` pattern).
- Barrier folding applies to every kept recording: passes a description has off cost a few composed barriers, not their own batches.
- The main ImageSpace client and the shape manifest remain for the water effects at the frame's start, menus and other frame kinds.

## Frame kinds and the UI stage (recipe plan, stage 5; 2026-11-16, `org-native/phases`)

### Builds switch in with their recordings

Before this stage an attach dropped every frame slot's schedule of the stage (`Scheduled::Absent`) until the new build's recordings were
made: 1 to 3 frames without the stage after every build. Now a CS stage's build is staged, recorded, and switched in whole.

- **ORG** (`GraphHost::StageEpochGraph`, `AttachStagedEpochGraph`; persistent-epochs.md, "Staged graphs"). The host's thread records a
  built graph before it runs, for recording requests that name its generation. The attached graph keeps submitting meanwhile, with its
  aliased bindings frozen (the staged graph's placement may move its transients), and takes no new recording.
- **Build manager** (`BuildManager::Desc::staged`: every CS stage's epoch; `CS_ORG_STAGED_BUILDS=0` attaches at once). A finished build of
  a stage is staged, not attached. What the build defers to its staging (`Builds::DeferStaged`) runs then: FrameAnchors keeps the staged
  build's snapshot and passes. What it defers to its attach runs at the switch (`BuildManager::SwitchStaged`).
- **The switch** (`Schedule` in FrameAnchors.cpp, at `Renderer::Begin`). While a build is staged, each Begin describes the frame against
  the staged snapshot and asks for that description's recording for every frame slot (`RequestEpochRecording` with the staged generation).
  When all four are done, the staged graph is attached and every slot's schedule switches to its recording, at that Begin. Until then
  every slot keeps the attached build's schedule; a description change in that time lags (the attached graph records nothing new). This
  runs in frames of every kind, so a build made during a load or a menu is in place for the next world frame. Logged as
  `[ORG] schedule: the <stage> segment switches in frame N (...): build B switched in whole with its recordings for every frame slot`.
- **Versions lag too.** A slot whose recording binds a version that was replaced keeps that recording (it holds the old version) until the
  recording of the new description is ready: the stage lags a frame, it is not blanked. A version published after Begin is still a
  mismatch (`StaleRecording`), checked only for a recording that was current at Begin.
- **A stage with nothing to record** (no client and no module step in its graph: the UI stage before the overlay registers) is absent, as
  before its first build, and its staged build switches in at once.
- The census adds `build switches` and `absent after it first ran` per stage. Every run below: 0 absent frames after a stage first ran.

**Description-off passes.** This stage kept them folded, arguing that a build per description costs a build and a switch for each
of the many description changes. That argument held only for a build per description. A build key per stage that changes only when
the set of passes a frame may want changes (not per frame) needs no build in a steady scene; see "Absent passes" below, which
replaces this choice.

### Recipes for the other frame kinds

`Recipes::KindHas`, `DescribeKind`, `DiffKinds` (Recipe.cpp). Each kind has its own ordered list of stages; a CS stage a kind does not list is
absent from its frames (not declared). The kind is decided at `Renderer::Begin` from events (stage 1 of the declared frame: the movie tick,
the loading screen's start, the thread, the engine's own freeze-background test and the player's 3D), never asked mid-frame.

| Kind | Recipe (* Drawcall Limit Fix's graphs; "tickets": anchors' own epochs) |
|---|---|
| world, and a frozen world's capture frame | start, shadows*, pre-depth, depth*, pre-main, opaque*, deferred+water, forward*, first person*, post, UI |
| menu over a frozen world | frozen composite and menu scene (tickets), UI (the menu UI stage since "The menu UI stage", below) |
| main menu | menu scene (tickets), UI (the menu UI stage since "The menu UI stage", below) |
| loading screen | loading model (tickets), UI (the menu UI stage since "The menu UI stage", below) |
| Bink movie | the engine's clear (ticket), UI (the movie's tiles and the overlay since the stage 5 movie part; the clear dropped since "The menu UI stage") |

The frame-kind line names the recipes and what changed: `[ORG] frame kind: kind loading screen -> world (frame 571); recipe loading model
(tickets) | UI -> start | shadows* | ... | UI; stages added: ...; removed: loading model`. Every stage is built at setup; a kind switch
changes which stages a frame declares, and the stages it keeps (UI) keep their schedules, so a switch blanks nothing.

### The UI stage

`SegmentId::Ui`, `Segment::DeclaredUi` (FrameAnchors.cpp, kDeclaredInfo), in every kind's recipe; `CS_ORG_UI_STAGE=0` turns it off. It holds the
Overlay anchor wholly (the ImGui overlay: its upload and draw passes, recorded in the stage's late batch from the run's draw list, a frame
input). It ends at a point: just before the present (Hooks.cpp), or just before HDR output or the capture is submitted, since both draw after
the overlay on the GPU.
- A new overlay target, or a font atlas page made or dropped (`ImGuiBasicRHI::PublishTextures`, answered before the run), is a new version
  published in the middle of the frame: that frame draws no overlay, and the stage's recordings take the version from the next boundary.

What it does not hold yet, and why:
- **Scaleform's displays**: in world-recipe frames since the stage 5 finish (below). In the other kinds they keep the Interface anchor's
  ticket.
- **HDR output and the capture** (tickets after the stage's commit). HDR output can copy into the back buffer through the swap chain's
  per-frame image, which a kept recording cannot bind; the capture checks its copy's submission right after its run.
- **The movie tiles and the Present anchor's clients** (Bink): in the stage since the stage 5 movie part (below).

### Measurements

perf-taa, native Vulkan, Perf-Fast, Tracy, one run each. Before: stage 4's final run (`s7fin2`). After: this stage's final build
(`st5perf`). The machine was in use during the after run: its frame rate and thread times are rough, and no conclusion is drawn from them.

| perf-taa | Before (measured) | After (measured, machine in use) |
|---|---|---|
| fps between the marks | 123.3 | 123.3 |
| Tickets a frame | 13.96 | 13.91 |
| `vkQueueSubmit` calls a frame (RHI; plus 1 external) | 24.06 | 24.01 |
| Entry / body barriers a frame | 506 / 811 | 505 / 811 |
| Render / submission / host thread busy, ms a frame | 3.04 / 2.26 / 1.17 | 3.15 / 2.71 / 1.39 |
| Overlay: ticket, entry / body barriers | anchor 1.00, 5 / 4 | UI stage 1.00, 5 / 4 |
| Recordings, schedule switches, tap errors, recipe findings in the steady window | 0, 0, 0, 0 | 0, 0, 0, 0 (recipe: 490 passes) |

The world frame's tickets are unchanged in number: the overlay's anchor ticket is the UI stage's ticket. The stage's value is in the other
kinds' frames and in the switch (below).

### Checks

One run each, `CS_TEST_BACKGROUND=1`, mismatches and recipe findings fatal, the final build:

| Run | Result |
|---|---|
| native-quick (DLSS registered) | passed, 0 tap errors |
| native-d3d12-quick | passed, 0 tap errors |
| native-river | passed, 0 tap errors |
| native-upscalers (DLSS, FSR, TAA, XeSS) | passed, 0 tap errors |
| native-interior | passed, 0 tap errors |
| native-validation (Whiterun, inventory, map, save and load, Dragonsreach) | passed, 0 tap errors; the known 20x `VUID-vkCmdDraw-None-09600` only |
| the loading-thread run | passed, 0 tap errors |
| first and third person (with the `start` shot) | passed, 0 tap errors |
| guest-quick | passed |
| main menu (Start On Save off, shot at main-menu + 300 frames) | menu shot correct, 0 tap errors; the harness marks it failed because it expects a world |
| a menu over a frozen world (JournalMenu through the harness's menu message) | passed, but the menu did not open: frozen world not reached |

Shots match stage 4's by eye. ORG's unit tests: all pass but HeadlessBenchmark (times out in Debug) and the lease tests (known to hang);
Pipelines timed out once while a CS build ran beside it and passed alone.

Frame kinds reached: Bink movie (the startup logo, frames 1 to 166 of every run), main menu (10 frames in every run; held 300 frames with
Start On Save off, shot `menu`), loading screen (every run; the validation tour's save and load too), world. Not reached: **a menu over a
frozen world**. The validation tour's inventory and map do not freeze the background, and a journal menu opened through the test harness's
menu message did not open (the shot shows the world); a menu opened by key would need focus, so it was not run.

Staged switches and absent frames (every run): every stage's build after setup was staged and switched in whole, 1 to 4 frames after its
recordings were first asked for; 0 frames a stage was absent after it first ran (`absent after it first ran 0 (session 0)` in every census
line of every run). The UI stage is absent only before the overlay registers (3 frames at startup).

The fatal paths fired three times during the work, each fixed before the final runs:
- The UI stage with no client recorded an empty graph (not replayable): a stage with no client and no module step is now absent.
- A font atlas page made in the middle of a frame (a new version after Begin): the overlay is not drawn that frame (`PublishTextures`).
- The validation tour's load: a staged post build bootstrapped while the staged build before it placed a transient, and kept that
  placement; its first recording found the binding moved (`Binding replacement requires a structural alias edit`). Fixed in ORG: a
  transient another graph placed is placed again by the next plan (persistent-epochs.md, "Staged graphs").

### What is still a ticket at a hook, by frame kind

*Updated by "The menu UI stage" (below).*

- **World**: the sun's queries (1.91), the decal tests, HDR output and the capture when they run (see stage 4 for why). A menu's scene over
  the live world (the inventory's item, the local map) runs on the EngineDraws and ImageSpace anchors' own epochs, and the world's own
  frames before Drawcall Limit Fix draws (the loading thread's first world frames) keep the shadow and frame clear anchors' tickets.
- **Loading screen, main menu**: none in the test runs; the menu UI stage is the frame's one ticket. A clear of a view the menu UI stage's
  places do not clear keeps its group anchor's ticket (none seen).
- **Bink movie**: none; the UI stage is the frame's one ticket.
- **Menu over a frozen world**: the menu UI stage is the frame's one ticket (the frozen composite, the displays, the present's clients, the
  overlay); its capture frame is a world frame (see "A frozen world's capture frame", the end of this document).

### What of the old native paths is still reachable (for stage 6)

*Updated by "The menu UI stage" (below).*

- The ImageSpace main client and the shape manifest: none in native mode with CS's stages. Guest mode keeps them.
- The Interface, Present and Overlay anchors' own epochs: none in native mode while the UI stages are on (`CS_ORG_UI_STAGE=0` brings them
  back). `Step::displayKindsOnly` and the UI stage's description per kind group are deleted.
- Per-anchor epochs still reached in native mode: EngineDraws and ImageSpace for a menu's scene in a world frame, EngineDraws for blood and
  precipitation, the shadow and frame clear anchors in world frames before Drawcall Limit Fix draws and for views no place or host clears,
  EngineCopy outside the stages' windows, MovieTiles, HDROutput, Capture, the decal and sun queries. `CanRun`/`RequestJoin` for their
  clients; the D3D11 replay of a group anchor's clears when its run does not submit.
- `AttachEpochGraph` for CS stages: only with `CS_ORG_STAGED_BUILDS=0`, or for a stage with no graph yet (setup's builds attach).
- Guest mode uses every anchor's own epoch (no CS stages), the main ImageSpace client and its manifest, and the D3D11 renderer for displays
  the graph refuses.

### Scaleform in the UI stage (stage 5 finish; 2026-11-16, `org-native/phases`)

The UI stage's steps are now Interface (Scaleform's displays), Overlay, and the point end. In a world-recipe frame (the world, a frozen world's
capture frame) Scaleform's displays are drawn by the UI stage: one graph, one kept recording per frame slot, one ticket with the overlay.
The Interface anchor's own ticket is gone from those frames.

- **Fixed instances.** Each display segment (a display, or each part of one between pushed render targets, filter passes and every 1024
  commands) is one Interface run, and the k-th run of the frame is instance k of the Scaleform client. There are 8 instances
  (`kInstances`, Scaleform/Passes.cpp), all declared every frame; the most runs any test frame had is 4 (the map). An instance has its own
  1024 gate entries (one run gate of 8 x 1024 entries) and its own run slots per surface pass. A run the engine does not make stays closed:
  its slots hold nothing and its draws read zero counts. A ninth run in one frame is a mismatch (`instance not declared`).
- **Passes.** Every surface (the framebuffer, HDR's UI texture, kMAIN, the four book pages, the four scratch images) has a pass, and a masks
  pass where its depth-stencil can be imported: 18 for each instance, 144 in all. They are made instance by instance
  (`ClientDesc::instanceMajor`, new): every pass of instance 0, then of instance 1. The graph keeps declaration order between passes on one
  target, so a later display draws over an earlier one on every surface, and a scratch image is written before the display that reads it.
  Each pass records in the stage's late batch from its instance's slot (the display list is a frame input).
- **A shared step for the world recipe only** (`Step::worldOnly`, new). In the loading screen and the menus the engine draws displays before
  and after the 3D scene (the loading screen's sequence: two displays, the shadow clear, the menu model, its composite, then the text), and
  those scenes are still anchor tickets: holding the displays until the UI stage's commit would draw them after the scene. So in those kinds
  the Interface anchor keeps its own epoch, and the Scaleform client is off in the UI stage's description there (its passes skip ahead and
  fold). The UI stage's description therefore differs between world-recipe frames and the others. Both are recorded ahead (the other one's
  recordings are asked for at every Begin), and a switch between them changes every frame slot's schedule at once, so the first world frame
  after a load draws its HUD (before that fix, 3 to 6 displays were left out at the switch). Only stages with a worldOnly step switch every
  slot at once: a description that alternates by frame (the post rows' ping-pong) keeps one schedule per slot.
- **Registrations.** A new texture read by a display registers the client again (to declare the read). That registration keeps the surfaces,
  slots and gate, so the schedule's older registration still draws it (`ClientDesc::matchesById`, new): the stage taps it as the registration
  its graph holds, and the anchor's own epoch counts it as executed. A surface made again (a resize, HDR's textures) is a new
  `ClientDesc::generation`: until the stage's build with it switches in, the display lags (`FrameAnchors::HeldNow`, new, names the
  generation the schedule holds). The client registers again only after a display's last run, never between two runs of one display
  (that was the "client was not in the Interface epoch" left out 4 times a run before).
- **No D3D11 fallback in native.** A display the graph refuses (no surface for its target, a mask on another depth-stencil, a run that did not
  execute, the path off, no target bound) is a mismatch (`refused`, a new tap error): fatal in development builds, logged once in release.
  The schedule's lag is not a refusal. In guest mode the D3D11 renderer still takes a refused display. Every run below: 0 displays left out.

Barriers: the UI stage's 144 passes are recorded at execution, so the ones with no display still record their barriers (a scratch image
moves between render target and shader read once per instance): its body barriers rise from 4 to 67 a frame, against the 10 entry and 7
body barriers of the Interface ticket it replaces.

perf-taa, native Vulkan, Perf-Fast, Tracy, one run each. Before: `st5perf` (the stage's first part). After: `s8perf2`. The machine was in use
in both runs: frame rates and thread times are rough, and no conclusion is drawn from them.

| perf-taa | Before (measured) | After (measured) |
|---|---|---|
| fps between the marks (rough) | 123.3 | 122.1 |
| Tickets a frame | 13.91 | 12.90 |
| `vkQueueSubmit` calls a frame (RHI; plus 1 external) | 24.01 | 22.97 |
| Entry / body barriers a frame | 505 / 811 | 504 / 866 |
| Interface ticket: entry / body barriers | 1.00 tickets, 10 / 7 | none |
| UI stage: entry / body barriers | 5 / 4 | 14 / 67 |
| Render `ExecuteEpoch`, Interface + UI, us a frame | 89.1 + 11.1 | 25.3 |
| Render / submission / host thread busy, ms a frame (rough) | 3.15 / 2.71 / 1.39 | 2.95 / 2.71 / 1.38 |
| Recordings, schedule switches, tap errors, recipe findings in the steady window | 0, 0, 0, 0 | 0, 0, 0, 0 (recipe: 628 passes) |

One run each, `CS_TEST_BACKGROUND=1`, mismatches and recipe findings fatal: native-quick, native-d3d12-quick, native-river,
native-upscalers, native-interior, native-validation (the same 21 validation lines as the build before: the known draw and back buffer
layout messages), the loading-thread run, first and third person: all passed, 0 tap errors, 0 frames a stage was absent after it first ran,
Scaleform census 0 left out. guest-quick passed. The main menu (Start On Save off): menu shot correct, 0 tap errors (the harness marks it
failed because it expects a world). The Bink movie (startup logo), main menu, loading screen and world kinds were reached in every run; a menu
over a frozen world was not reached (no background way to open one).

What is still a ticket at a hook after this:
- **World**: the sun's queries (1.91), the decal tests, the water effects (the ImageSpace main client) at the frame's start, HDR output and the
  capture when they run.
- **Loading screen, main menu, Bink movie, menu over a frozen world**: as in the first part, and Interface with them (see the worldOnly step
  above for why).
- **HDR output** could be a step of the UI stage in native mode (the back buffer is one graph-owned version there, so a kept recording can
  copy into it), but its dispatch, copy and clear are not gated: in a frame where the step does not fire they would still run with zero
  constants. They need gate entries and a declared predicate first. HDR is off in every test configuration, so it was not changed untested.
- **The capture** stays its own ticket: its copy's readback is planned in the pass's preparation (the readback service takes the request
  through a lifecycle effect when the copy is submitted), and a pass in a kept recording's late batch has no preparation per submission
  (`PreparedPass::RecordAgain`), so the plan cannot be made there without an ORG addition. Separately, the capture's own check ("capture N
  was not in the epoch submitted at its capture point; D3D11 copies it") fires in native runs before and after this change: the harness's
  shots are taken by the D3D11 copy. Not investigated here.
- **The movie tiles and the Present anchor's clients** (Bink): moved into the UI stage by the stage 5 movie part (below). The Present hook
  fires after the displays (the frame sequence census), so the stage keeps their order.

Not done in this stage (the plan's stage 5 items 3 to 5): the non-world kinds' scenes as CS stages (the menu scene, the frozen-world
composite, the loading model, the movie), the world leftovers (the water effects as start-stage steps, the decal queries and the sun queries
as fixed instances or stage passes), and so the ImageSpace main client, the shape manifest, the per-anchor epochs and `CanRun`/`RequestJoin`
remain reachable as listed above.

### The movie's tiles in the UI stage (stage 5, movie part; 2026-11-16, `org-native/phases`)

The UI stage's steps are now Interface (Scaleform's displays, world-recipe frames), Present, Overlay and the point end. The Present anchor is a
whole step: it has no epoch of its own in native mode with the UI stage on. Its clients are a Bink movie's tiles (`cs.bink`) and the GPU load
diagnostic (`CS_GPU_LOAD`). The swap chain's Present hook taps it after the displays and before the overlay, as before.

- **The tiles as a frame input.** The movie's tick queues the decoded frame; the Present tap's commit stores it in the run's slot (`RunSlots`,
  moved to the submitted run by the stage's commit); the tile pass records in the stage's late batch from that slot. The pass now has a
  revision (empty when it records late): a pass without one made the stage's recording not replayable, which is fatal (the first build's
  first run hit it). Guest mode keeps the anchor's own epoch, with the pass prepared again every run as before.
- **The tick asks the stage, not the anchor.** A blocking tick runs between two frames, before the frame's Begin, so `HeldNow` (an open
  window) cannot answer there. `FrameAnchors::InStage(id, anchor)` (new) says whether the last frame's schedule of the stage that holds the
  anchor holds this registration (before the first frame, its attached build). Bink's `CanRun` check is guest mode's only.
- **The session's first movie frame is not drawn.** The UI stage's first schedule switches in at frame 2 (frame 1's Begin asks for its
  recordings), so the frame the first tick queued has no stage to draw it: one `[E] Bink: a movie frame queued for its anchor did not draw`
  per session. Before, the Present anchor's own epoch drew it. 166 of the logo's 167 frames are drawn by the graph.
- **Bink frames keep Scaleform off.** The step that holds the displays is now `displayKindsOnly` (was `worldOnly`), held where
  `Recipes::UiDrawsDisplays` says the UI stage draws the displays: the world recipe only. Adding the Bink kind was measured and undone: the
  logo's frames draw no displays, and with the displays' 144 passes on the startup window's body barriers rose from 11 to 73 a frame.

The frame sequence census of a Bink frame (startup logo, `CS_ORG_FRAME_SEQUENCE`):

```
before: clear Native::BackBuffer | EngineFrameClear (depth0) -> its ticket | Present [cs.bink] -> its ticket | Overlay -> its ticket
after:  clear Native::BackBuffer | EngineFrameClear (depth0) -> its ticket | Present [cs.bink], Overlay (held) -> CS stage: UI
```

The main menu's frame (held 300 frames with Start On Save off) has the loading screen's shape: kMAIN's clear, a display, the shadow clear,
kMAIN's and depth0's clears, the depth copy (EngineCopy), the menu scene's colour (EngineDraws), its composite (ImageSpace), depth0's
clear, two displays, then the UI stage. So the menus and the loading screen need the same work to become one stage (below).

Per frame kind, census windows (one run each, the machine in use; fps and thread times rough). Before: `u7q`/`u6mm`/`s8perf2` (the stage 5
finish build). After: `a3q`/`a3mm`/`a3perf2` (this build). The startup window mixes the Bink frames with the first main-menu frames.

| Window (frames by kind) | Tickets a frame | `vkQueueSubmit` a frame | Entry / body barriers a frame |
|---|---|---|---|
| startup, before (Bink 166, main menu 10) | 3.4 | 3.4 | 35.5 / 10.9 |
| startup, after (Bink 166, main menu 25) | 3.2 | 3.1 | 44.2 / 18.8 |
| one Bink frame (sequence census) | 3 -> 2 | | |
| main menu, before / after | 10.9 / 10.9 | 10.3 / 10.0 | 164.2 / 109.5 both |
| loading screen, before / after | 15.0 / 15.0 | 13.1 / 13.3 | 220.0 / 125.0 both |
| world (perf-taa), before / after | 13.0 / 13.0 | 15.0 / 15.0 | 507 / 871 -> 505 / 867 |
| perf-taa fps between the marks (rough) | 122.6 -> 123.5 | | |

The startup window's entry barriers rise because the UI stage enters the movie texture and the framebuffer every Bink frame, and the window
holds more main-menu frames. The main menu, loading screen and world are unchanged: no Bink there.

Checks, final build, one run each, `CS_TEST_BACKGROUND=1`, mismatches and recipe findings fatal: native-quick, native-d3d12-quick,
native-validation, native-interior, the loading-thread run (with a `loading` shot at loading-screen + 400 frames), first and third person,
guest-quick and guest's loading shot, native-perf-taa: all passed, 0 tap errors, 0 recipe findings, 0 frames a stage was absent after it first
ran, Scaleform census 0 left out. The main menu (Start On Save off): menu shot correct, 0 tap errors (the harness marks it failed because it
expects a world). The native and guest loading shots match (the text and the level bar). The startup logo, main menu, loading screen and
world kinds were reached in every run; a menu over a frozen world was not (no background way to open one). The lines "submitted between
the first tap and the commit" at the world's first frames and the `PublishedResources` `[C]` line are in the stage 5 finish build's runs too.

What is still a ticket at a hook, by kind, after this:
- **World**: as before (the sun's queries, the decal tests, the water effects, HDR output and the capture when they run). The water
  effects are the start stage's since "What replaced the main ImageSpace client" (below).
- **Bink movie**: the engine's clear of the main depth (EngineFrameClear, 1 a frame).
- **Main menu, loading screen, menu over a frozen world**: the scene (frame and shadow clears, EngineDraws, EngineCopy, ImageSpace) and
  Scaleform's displays (Interface), as before.

Why the menus' and the loading screen's scenes are not stages yet (stage 5 items 1 and 2, not done):
- Their displays are drawn before and after the scene. One stage for the whole kind (the UI stage with the scene's steps between a
  before-scene and an after-scene Interface step) is the simplest layout that keeps the order. It needs a step that takes taps only until a
  later step tapped (the before-scene displays), and per-step instance ranges for the Scaleform client (instances 8-11 before the scene, 0-7
  after, the first range off in world frames so its passes fold).
- The scene's ImageSpace composite runs through the main ImageSpace client, built from the shape manifest, in batches that reuse their gate
  entries per run: it cannot be a step of a stage. It needs CS passes of its own, as the post chain has (ChainSteps.inl).
- The menu scenes' draws (`cs.menu-scenes.*`) are recorded ahead from the latest run (a revision): as frame inputs they need a per-run store
  and the late batch, as the water's draws have.
- The engine's clears repeat inside the scene (depth0 three times, kMAIN twice). As load operations of the first writing pass they need
  changes in those passes (DCLF's menu scene and shadow mask, Scaleform's masks); as clear passes of the stage the repeats collapse to the
  first, and the last depth0 clear (before the after-scene displays) is then left to Scaleform's own stencil clears.

What of the old native paths is still reachable: as listed in the stage's first part, without the Present anchor's own epoch (gone in native
with the UI stage on; guest mode keeps it).

### The menu UI stage: the loading screen's and the menus' scenes (stage 5, scenes; 2026-11-16, `org-native/phases`)

The loading screen, the main menu and a menu over a frozen world are one CS stage each frame: the menu UI stage (`SegmentId::MenuUi`,
`Segment::DeclaredMenuUi`, FrameAnchors.cpp, kDeclaredInfo). A Bink frame is the UI stage alone. Every kind's recipe has one stage now.

| Kind | Recipe |
|---|---|
| world, and a frozen world's capture frame | start, shadows*, pre-depth, depth*, pre-main, opaque*, deferred+water, forward*, first person*, post, UI |
| loading screen, main menu, menu over a frozen world | menu UI |
| Bink movie | UI |

**Two UI stages, two graphs.** The menu UI stage is a stage of its own, not steps of the UI stage: a world frame's UI graph does not carry the
menu scene's passes, and the menu UI graph does not carry the world UI's (the per-kind pass counts are below). Both hold the Interface, Present
and Overlay anchors wholly; the one in the frame's recipe takes their taps (`DeclaredOfAnchor`). The Interface anchor has no epoch of its own in
native mode any more. Each UI stage's schedules are kept current in frames whose recipe lacks it (`KeptAhead`: the description its frames would
have is recorded ahead for every frame slot, and installed when ready), and both are recorded at setup (`PrimeStages`). A kind switch then
finds the stage's recordings ready. Every other CS stage gets a first schedule recorded ahead for each frame slot that has none, in frames
whose recipe lacks it (only for the slots of the frame's parity, since a description carries the slot-parity variants): the validation
tour's load had one frame with pre-depth absent at the world's first frames (slot-by-slot first recordings), which this removes. After that
those stages lag at worst, never blank.

**The steps**, in the engine's order (the frame sequence census of the loading screen and the main menu; the frozen composite by code):

| # | Step | What it holds |
|---|---|---|
| 0 | ImageSpace (`cs.engine.imagespace.frozen`) | the frozen frame's composite (ISCopy kMENUBG into the framebuffer, Main::Render) |
| 1 | EngineFrameClear (`...frame@menu-start`, `...shadow@menu-start`) | clears at the stage's start |
| 2 | Interface, instances 8 to 15, `untilLater` | the displays before the scene |
| 3 | EngineFrameClear (`...@menu-scene`) | the scene's clears |
| 4 | EngineDraws (`cs.menu-scenes.menu-depth`) | the scene's depth batch |
| 5 | EngineDraws (`cs.shadow-mask.menu`) | the scene's shadow mask |
| 6 | EngineDepthCopy (`cs.engine.skeleton.depth-copy`) | the main depth into kPOST_ZPREPASS_COPY (ungated, as in pre-main) |
| 7 | EngineCopy (shared) | kMAIN into its copy |
| 8 | EngineDraws (`cs.menu-scenes.menu-colour`, `cs.menu-scenes.local-map*`) | the scene's colour |
| 9 | ImageSpace (`cs.engine.imagespace.menu`) | the scene's composite (the HDR effect or the copy of kMAIN into the framebuffer) |
| 10 | EngineFrameClear (`...@menu-after`) | the main depth's last clear |
| 11 | Interface, instances 0 to 7 | the displays after the scene |
| 12 | Present | the present's clients |
| 13 | Overlay | the overlay |
| 14 | point | the commit, just before the present (or HDR output, the capture) |

**The framework pieces.**
- `Step::untilLater`: a step takes its anchor's taps only until a later step of its stage tapped this frame; the anchor's next tap goes to the
  stage's next step at that anchor (`StepTakesTaps`, `TapStepAt`, used by the whole-anchor and shared-anchor tap paths and by `HeldNow`).
- `Step::firstInstance`, `Step::instanceCount`: a step makes and taps a range of a client's instances; the step's k-th tap is instance
  `firstInstance + k`. Scaleform has 16 instances: the UI stage's Interface step makes 0 to 7, the menu UI stage's 8 to 15 before the scene
  and 0 to 7 after it. The before-scene range does not exist in the world's UI graph at all: it is another stage's graph.
- A step names up to four clients, and a name ending in `*` names every client whose id starts with the rest.
- `FrameAnchors::StagePlace`: where in an open stage work handed over outside a tap belongs (below).

**Clears.** The engine clears, in a loading screen's frame: kMAIN, the motion vectors, the normals and the main depth before the first display;
a shadow map slice and the main depth before the scene; the shadow mask after the depth batch; kMAIN again after the shadow mask; the main depth
again after the composite. The main menu's are a subset. They are declared clear passes, not load operations:
- the main depth is genuinely cleared three times a frame, each with a reader before the next: the displays' masks before the scene (UI::
  DrawInterface binds kMAIN's depth-stencil), the scene's depth batch, the displays' masks after the scene. kMAIN twice.
- the first writer of every cleared target (the menu depth, shadow mask and colour batches, the displays) draws from a per-run list that may be
  empty in a frame (a loading model not loaded yet), while a later pass still reads the target (the colour batch reads the shadow mask, the
  composite reads kMAIN). A load operation on that writer would leave the target uncleared in such a frame.

Each of the frame and shadow clear groups has a client at each of three places of the stage (the steps 1, 3 and 10). Each place has its own gate entries (place x entries + entry) in a
gate and colour latch of its own, since one view is cleared at several places in one frame. The engine's call stages its clear into the place
`StagePlace` names: of the stage's steps that name a place, the latest at most one step after the stage's last tapped step this frame, or the
first. In the loading screen's frame that gives: the first clears at the start; the shadow slice and the main depth after the displays at the
scene's place; the shadow mask and kMAIN after the depth batch also at the scene's place (moved before the depth batch, which touches neither);
the last main depth after the composite. The places clear kMAIN, the motion vectors, both normals targets, the shadow mask, the main depth and
the shadow maps' slices; a clear of any other view in those frames keeps its group anchor's ticket. The places' clients are segment-only (the
group anchors' own graphs leave them out).

**A Bink frame is one ticket.** The engine clears the main depth for a Bink frame before that frame's `Renderer::Begin`, outside every stage's
window (the frame sequence census put it after the previous present, and `StagePlace` found no open stage). Nothing reads the main depth in
a Bink frame (the movie's tiles and the overlay draw without it), and every other kind clears it before its first read (the menu UI stage's
places, Drawcall Limit Fix's Z-prepass). Native mode drops that clear (Engine/Skeleton, `Take`), as it drops the reflection faces' clears the
sky's load operations replace. A Bink frame is now the UI stage alone.

**The depth copy.** The menus' depth copy reaches the native context's CopyResource with other handles of the main depth and its copy than the
engine's table holds, so it was not recognised and ran as external work: inside the menu UI stage's window that would have run ahead of the
stage's depth batch on the GPU. The skeleton now compares images (`SameImage`: the same handle, or the same image behind both, through
`Native::DescribeResource`), and the copy is the stage's pass.

**Frame inputs.** The menu scenes' draws (`cs.menu-scenes.*`) and the shadow masks (`cs.shadow-mask.*`, both views) are frame inputs now: the
commit stores the run's draw list (`RunSlots`), and the pass records it at execution in the stage's late batch (as the water's draws do), so the
kept recordings never change with the draws. Before, they were recorded ahead from the latest run, a revision in the stage's description.
The menu composites are recorded at execution with the CPU's gate answer (`gpuGated = false`): every hook of the stage runs before its commit.
The menu composite client `repeats` (the HDR effect and a copy are two taps a frame). The frozen composite is its own client, at the stage's
first step.

**What stays where it was.** In a world frame a menu's scene over the live world (the inventory's item, the local map) still runs on the
EngineDraws and ImageSpace anchors' own epochs: the world's UI stage does not hold the scene. Guest mode keeps every anchor's own epoch.

**Passes per stage graph** (the build lines `[ORG] frame segments: a build of the <stage> holds N passes`): the world's UI stage 146 to 148
(Scaleform's instances 0 to 7, 18 passes each, the movie's tiles, the GPU load diagnostic, the overlay); the menu UI stage 341 to 380
(Scaleform's 16 instances, the three clear places, the scene's clients, the composites). Neither carries the other's passes.

**Measurements.** Census windows, one run each (before: `b0mmseq`, `b0loadseq`, the fixed post targets build; after: `e1mml`, `e1loadseq`,
`d1perf`). Barriers: entry / body as the lists are built / body recorded (less those folded at execution). Machine in use: thread times are
rough and no conclusion is drawn from them.

| Kind | Tickets a frame | `vkQueueSubmit` a frame | Barriers a frame (entry / body built / body recorded) |
|---|---|---|---|
| main menu, before | 10.9 to 11.1 | 10.1 to 10.7 | 132 / 58 / 40 |
| main menu, after (pure 300-frame windows) | 1.0 | 1.0 | 40 / 201 / 64 |
| loading screen, before | 15.0 | 13.2 to 13.6 | 187 / 72 / 54 |
| loading screen, after (per menu UI ticket, a window mixed with the world's first frames) | 1.0 | 1.0 | 40 / 201 / about 64 |
| Bink movie frame (sequence census) | 2 -> 1 | 2 -> 1 | |
| startup window (Bink 166, main menu 25 before, 134 after) | 3.2 -> 1.0 | 3.0 -> 1.0 | 39.6 / 11.5 / - -> 25.8 / 127.4 / 31.4 |
| world (perf-taa), before / after | 12.91 / 12.91 | 17.88 / 17.88 (all queue submissions) | 503 / 899 / 806 both |

Entry plus recorded body barriers a frame fall from 172 to 104 in the main menu and from 241 to about 104 in the loading screen. The recorded
body barriers rise because the passes of 16 Scaleform instances, the clear places and the scene are in one recording: most skip and fold at
execution (137 a frame), the rest record their barriers. The submission thread's busy time a main-menu frame is 0.41 ms against 0.74 ms before
(rough). perf-taa fps between the marks 120.5 against 122.4 (rough, machine in use).

**Checks** (one run each, `CS_TEST_BACKGROUND=1`, mismatches and recipe findings fatal): native-quick, native-d3d12-quick, native-validation
(the known 20 `VUID-vkCmdDraw-None-09600` and the back buffer layout lines), native-interior, the loading-thread run, the loading shots native
and guest (they match: the text and the level bar), first and third person, guest-quick, native-perf-taa: all passed, 0 tap errors, 0 recipe
findings, 0 frames a stage was absent after it first ran (session counts included), Scaleform census 0 left out. The main menu (Start On Save
off, held 1500 frames): menu shot as before (the harness marks it failed because it expects a world); the menu's emblem is missing in native
shots before and after this change alike (guest draws it), and the mist is lighter than in two earlier native shots: not investigated.

**A menu over a frozen world: reached, and stopped in the capture frame.** The harness's menu message with the menu's real name (`Journal
Menu=show`, not `JournalMenu`) opens the pause menu in the background. Its first frame is the frozen world's capture frame (the world recipe),
and there the engine's end-of-frame effects run from kMAIN into kMENUBG (target 14), not into the final target; the post stage's check
(`CheckEndOfFrame`, "Fixed post targets") stops the game in development builds. This predates this stage: the capture frame was never
reached before. It needs the post stage to copy its final target into kMENUBG in a capture frame (a copy step opened at the end-of-frame
entry when the final target is kMENUBG), and the check to accept that target. Not done; so the menu UI stage's frozen composite step (step 0)
and the frozen kind's scene were not exercised. *Done since: see "A frozen world's capture frame" at the end of this document.*

## What replaced the main ImageSpace client (2026-11-16, `org-native/phases`)

In native mode with CS's stages every ImageSpace draw is now a CS step's pass: the world steps and the post chain (as before), the start
stage's water effects, and the menu composites. The main client (`cs.engine.imagespace`), the clients built on first sight, the shape
manifest (`ImageSpaceShapes.json`) and the per-draw shape capture (ReadShape, ReadViewport, ReadComputeShape) are not set up there; any
other ImageSpace draw or dispatch is `NoPass` (fatal in development builds, one log line in release), with what the engine bound named in
the message for the diagnosis. To make that true for calls outside the hooked scopes, every ImageSpace call is a scope of its own in this
mode (`OwnsScope`), so an effect CS does not cover reaches the check instead of D3D11. Guest mode keeps the main client, the clients built
on first sight and the manifest unchanged.

### Inventory: what the main client still drew

From the manifest of the last sessions and the logs of the stage 5 runs (`a3*`, `u6*`), by frame kind:

| Kind | Draws | Shader, targets |
|---|---|---|
| world (river) | the water displacement update (31385 / 32176) | ISWaterDisplacementTexOffset into one of kWATER_DISPLACEMENT / its swap from the other; ISWaterDisplacementWadingRipple into the same, one draw a ripple, its quad pulled by its vertex stage; ISWaterWadingHeightmap from one into the other, 0 or more a frame |
| world (river, cell loads) | Main::RenderWaterEffects' flow copies (35561 / 36560) | ISCopySubRegionCS, a loaded flow texture into a rectangle of the flow map; up to about 24 in one frame seen |
| world (a load) | the player's face tint | ISAlphaBlend into kPLAYER_FACEGEN_TINT, 16 tint masks; refused before (unset layers hold the black default texture, which the main client took for an engine target), so it drew nothing in native |
| main menu, loading screen | UI3DSceneManager::Render's end-of-frame call (RenderEndOfFrameEffects with kMAIN as source and kFRAMEBUFFER as final, when the scheme has an ImageSpace override) | the HDR effect: ISHDRDownSample4 kMAIN into kHDR_DOWNSAMPLE0, the luminance levels, the last level (16Lum, either parity), ISBrightPassBlur15 into kHDR_BLURSWAP, ISBlur15 into kHDR_BLOOM, ISHDRTonemapBlendCinematic into kFRAMEBUFFER |
| main menu, loading screen | the copy of kMAIN into the framebuffer (a menu scene without override, or UI's render after a menu drew into kMAIN, SE 0x140ebf510) | ISCopy kMAIN into kFRAMEBUFFER |
| menu over a frozen world (by code, not reached) | Main::Render's frozen composite (SE 0x1412e2550) | ISCopy kMENUBG into kFRAMEBUFFER, alpha write mode 1 |

Nothing else reached the main client or D3D11 in the test runs (the "left to D3D11" summaries name only the face tint).

### The water effects: start stage steps (`StartSteps.inl`)

One client, `cs.engine.imagespace.start`, at a new step of the start stage (the ImageSpace anchor, after SkyReflections, before the point
end). RenderWaterEffects runs after the last face's sky in every world frame (frame sequence census), inside the stage's window.
- **The displacement update** (ImageSpaceEffectWaterDisplacement's Render, SE 0x141303da0, AE 0x1414ef370, decompiled): each child effect
  writes its texture 0 and reads its texture 1. The texture offset and every ripple write the pair's target held at SE 0x143283ae0 / AE
  0x1433dcf10 (read at the update's first draw: the update may reset the pair before it renders); the pair swaps; each heightmap step then
  writes the other target and swaps again. The hook keys each draw by effect, the target it writes (that plan) and its instance. A check
  compares the plan with the target the engine binds, once a session: no difference in the final runs.
- **The flow copies**: the k-th copy of the frame is instance k; its loaded texture is a frame input by heap index (the latch's Source).
- **Fixed instances** (the counts are the engine's late decisions): 8 ripples per target, 4 heightmap steps per target, 32 flow copies;
  58 passes. All are on in every description and gated: an instance the engine does not use stays closed and does nothing on the GPU. More
  in one frame is `NoPass`. The client `repeats`: a tap per update and per copy, each opening only its own instances' entries.
- Not built: the rain ripple, rain heightmap, blend, smooth and normals steps and the clear (shaders 86, 89, 91 to 94). The decompiled
  Render does not draw them and no session's manifest has them. A draw of one is `NoPass`.

The ImageSpace anchor's own ticket in world frames is gone (river windows with wading: 1.00 image space tickets a frame before, 0 after).
The cost is the start stage's body barriers, 137 to 178 a frame: the gated passes record theirs every frame, and the 32 flow copies write one
UAV. Description rows per frame would not do: a new description lags a frame or two, and a flow copy that lags is lost (the engine pops it
from its queue).

### The menu composites (`MenuSteps.inl`)

One client, `cs.engine.imagespace.menu`, on the ImageSpace anchor's **own epoch**: the scenes of those kinds are still tickets on their
anchors' epochs, and a run at the end of each engine call keeps today's GPU order with no new machinery. It registers at the session's first
frame (`ImageSpace::OnFrameBegin`, from the frame driver), so its build is attached before the main menu. Passes (36 at 1920x1080 with 4
luminance levels): the HDR effect on the engine's targets (downsample, levels, the last level by the luminance shader or the light
adaptation of either parity, the bloom of each radius, the tonemap and its fade form of either parity into the final target); the copy of
kMAIN into the framebuffer; the frozen copy of kMENUBG; the face tint. The variant comes from data only: RenderEndOfFrameEffects' source
and final target (PostChain's detour calls `OnMenuEndOfFrame` outside the world chain), RenderEffect's source and destination (99025 /
105676, hooked for its arguments), and the luminance parity global. The face tint's masks are frame inputs by heap index (the black default
texture of an unset layer too); a slot holding an engine render target would read the null texture (none seen).

Not built (`NoPass` if reached): ISSimpleColor's stencil fill and the UI form of TAA (UI3DSceneManager::Render with the engine's TAA on;
CS has no port of ISTemporalAA_UI), other slot effects inside a menu scene's end-of-frame call, the local map's edge pass, and the console
screenshot's and the debug views' copies.

### The first movie frame

The UI stage's first schedule is now recorded at setup (`FrameAnchors::PrimeStages`, after setup's build point): the description of the
first frames' kind (no displays), recorded for every frame slot on the host's thread while setup waits, and installed as each slot's
schedule. The first Begin finds it current, so frame 1 draws the stage, and the `[E] Bink: a movie frame queued for its anchor did not draw`
line is gone. With the stage present at frame 1, the first frame's initial layouts count as "submitted between the first tap and the commit
of the UI stage" (16 warning lines at frame 1).

### Measurements

perf-taa, native Vulkan, Perf-Fast, one run each, the machine in use (fps and thread times rough). Before: `a3perf2` (and `u6river`,
`a3mm`). After: `m4perf` (and `m3riverseq`, `m4mm`).

| Census | Before (measured) | After (measured) |
|---|---|---|
| world (perf-taa): tickets / `vkQueueSubmit` a frame | 13.0 / 15.0 | 13.0 / 15.0 |
| world (perf-taa): entry / body barriers a frame | 505 / 867 | 510 / 912 |
| start stage: entry / body barriers | 8 / 137 | 10 / 178 |
| river, wading windows: tickets, image space tickets a frame | 14.0, 1.00 | 13.0, 0 |
| main menu: entry / body barriers (tickets 10.9 both) | 164 / 109 | 132 / 58 |
| loading screen: entry / body barriers (tickets 15.0 both) | 220 / 125 | 187 / 72 |
| perf-taa fps (rough) | 122 to 124 | 122 |
| steady window: recordings, switches, tap errors, recipe findings | 0, 0, 0, 0 | 0, 0, 0, 0 (recipe: 688 passes in the river) |

The perf-taa scene has no water effects: its world tickets do not change, and its barriers rise by the start stage's new passes.

### Checks

One run each, `CS_TEST_BACKGROUND=1`, mismatches and recipe findings fatal: native-quick, native-d3d12-quick, native-river (with the frame
sequence census), native-validation (the same 20 `VUID-vkCmdDraw-None-09600` as before), native-interior, the loading-thread run, the
loading shot, first and third person, guest-quick and guest's loading shot, native-perf-taa: all passed, 0 tap errors, 0 recipe findings, 0
frames a stage was absent after it first ran. The main menu (Start On Save off): menu shot as before, 0 tap errors (the harness marks it
failed because it expects a world). One native loading shot showed the main menu's mist where guest's did not; a second pair showed none in
either (timing, not reproducible). Not reached: a menu over a frozen world (the frozen copy), UI3DSceneManager with the engine's TAA on,
ripples and heightmap steps past their first instances. The fatal path fired once during the work: the displacement target was first read
from the wrong variable (the plan check named it), fixed before the final runs.

## Absent passes (recipe plan, stage 6; 2026-11-16, `org-native/phases`)

A pass that is off is not handed to ORG, wherever a rebuild's latency on enable or disable is acceptable. Builds are staged (stage 5), so
a rebuild never blanks a stage: the old build keeps drawing until the new one switches in with its recordings. Only passes that must answer
on demand stay present and are chosen per frame by their gates.

### The build key

Each CS stage has a build key (`BuildKey`, FrameAnchors.cpp): per client registration (by its desc, so a client registered again starts
whole), whether its passes are present and which of its rows; and which module steps (light culling) are present. A client the key does not
list (one registered since) is present whole. Instances are never cut: an instance count is a late engine decision, and an empty instance
skips its run at execution (below).

- **At the build.** A build reads the stage's `nextKeys` when it starts (`PrepareForBuild`) and keeps it with the build: the staged build's
  key (`stagedKeys`) and the attached one's (`declaredKeys`). `GatherDeclared` leaves out every pass, module step and shared-constants upload
  the key does not have.
- **In the description.** `DescribeDeclaredFrom` cuts the frame's description to the build's key: a row, client or module step the key left
  out is off. The frame's own description (uncut) is kept beside it (`frameWanted`): a tap whose work the build left out is lagged work, as a
  lagging schedule's is, and the frame counts as lagging.
- **At Begin** (`UpdateKey`, after the stages' schedules). The stage is described as a key (`FrameAnchors::DescribingKey()`): every variant
  a frame may pick with the settings now (both parities), both kind groups (the UI stage), the rows a hook asked for, and the passes that
  must always be present. It is described against the clients registered now (the published snapshot), so a new registration is seen at
  once. Each wanted client, row and module step is stamped on the stage's own clock, which runs only in frames whose recipe has the stage
  (at most 0.1 s a frame). The key keeps what was wanted within 10 s of that clock.
- **When a build is asked for.** When the key wants something the stage's newest build (staged, else attached) left out: at once, from
  that Begin; frames meanwhile lag as before. To drop unused passes, only when that build's key was provisional (the first key; a client
  registered since), at a load, or at a frame-kind switch. Otherwise the key is only stored for the stage's next build, whatever asks for
  it. A build already asked for with a key that covers the new one is not asked for again. The key is logged
  (`[ORG] absent passes: the <stage> asks for a build at frame N (<why>): present now: ...; absent now: ...`), counted (`key builds` in the
  declared frame's census line), and the switch line names the frames from the key's request to its switch-in.
- **Per-frame fields stay out of the key.** A field that changes every frame (a parity, a ping-pong target) is in the key whole: the key
  description sets both values. Otherwise each frame would ask for a build and builds would supersede each other forever.
- `CS_ORG_ABSENT_PASSES=0` keeps every pass (A/B runs).

Rows: each client of a stage has its own rows. The world water client's rows overlapped the scope rows (both started at 0), so the SSR
downsample was never row-off; the water rows now follow the scopes' (`waterRowBase`).

A hook's request (a step drawn while its row is off) keeps the row on in the descriptions for 300 frames after the last request
(`kRequestFrames`), not until the next registration as before, so an unused effect can leave the key.

### What is absent when off

| Passes | Off when | How |
|---|---|---|
| post slot effects: world map, refraction, FXAA, depth of field, radial blur, full-screen blur, get hit (both places) | the slot's effect is not active, or not in its place (usesLDR) | rows (`ChainRows`) |
| HDR effect against its copy | the HDR switch | rows |
| the VL composite | volumetric lighting off | rows |
| TAA | CS's method is not TAA | rows |
| the dynamic-resolution upsample | no dynamic resolution | rows |
| bloom radii other than the current one | the radius | rows |
| the upscaler evaluate | the method is not an SDK upscaler | its own client `cs.upscaling.evaluate`, declared by `post.upscaler` |
| the Effects11 tonemap owner | Effects11 does not own the tonemap | the client unregisters itself when inactive (as before) |
| SSR (downsample, the ray march's two forms, blur) | the helper off; the downsample and blur switches pick the forms | rows (`WorldScopeRows`) |
| the SAO composite forms, and its blurred or raw reflection inputs | the SAO and fog switches; the SSR blur switch | rows |
| the raw AO's temporal or plain form | the temporal switch | rows |
| IBL, Terrain Blending, Volumetric Shadows | their `declared` (active, directional shadows) | clients |
| light culling | its declared | module step |

While an upscaler was just chosen and the evaluate's pass is not in the schedule yet, the Upscale client does not copy the unwritten upscaled
texture into kMAIN (`FrameAnchors::CanRun` on the evaluate client at the commit).

### What stays present, chosen per frame by gates

| Passes | Why present |
|---|---|
| the screenshot copy (one pass: the final target into kSAVE_GAME_SCREENSHOT) | a save asks for it with at most a frame of notice; a late copy leaves a stale thumbnail. Its row is on in every description, not only in the key, because the flag may be set after Begin read it. Recorded at execution: a frame that does not copy skips it, its barriers folded |
| both tonemap forms (plain, fade) | the fade starts and ends on a frame |
| both forms of the last luminance level (luminance, light adaptation) | the adaptation's reset lasts one frame |
| the engine-counter parity variants: last luminance level and bright pass by luminance parity, the VL composite by the VL counter, the normals target (n0/n1) in SSR, SAO and TAA, the water blend's history | the engine picks them per frame |
| TAA's and the raw AO's history pairs (both slot parities) | the frame slot's parity picks one; the other is off in the description (`SetSkipWhen`) and folds |
| the six reflection faces, Scaleform's 8 instances, the water effects' ripple, heightmap and flow copy instances | late engine decisions; an empty instance skips at execution |
| the snow pass and its copy | the snow flag changes with the weather |

**Bloom.** `ImageSpace::Bloom` is now a history (kept across frames, cleared once), not a transient. In a frame whose bloom lags (a new
radius absent until its build switches in) nothing writes the bloom, and the always-present tonemap reads it. Keeping every radius present
would cost 21 passes in every frame to cover a rare change; a history costs one target that is not aliased, and a lagging frame shows the
last bloom drawn, which is almost the same image. `BloomSwap` stays transient: it is written and read only within the radius's own passes.

### Empty instances record no barriers

ORG (`FoldBarriersOfSkipped`, RhiExecution.h): a late batch (recorded at execution) asks each pass first whether it skips the run
(`SkipsAtExecution`, `SkipsAgain`), and folds the barriers of the passes that skip into the next pass that runs, as a list built with passes
skipped ahead does (stage 4's fold, now one function for both). Every backing still ends in the state the plan gives it, so the ledger's
commits and the next stage's entry barriers are unchanged. A batch whose fold would compose an ownership transfer or overlapping different
ranges keeps its barriers. A kept late batch keyed by its skip answers stays consistent: the fold depends on the answers alone. The census
line counts what was left out (`late barriers folded at execution`).

Passes that now skip at execution with their barriers folded: Scaleform's display passes (`SkipsAtExecution`, new: no display drew on the
surface in this instance), the sky faces the engine did not draw, the upscaler evaluate, the gated copies and clears, and the start stage's
water effect instances (ripples, heightmap steps, flow copies), which are no longer GPU-gated (`gpuGated = false`): recorded at execution with
the CPU's gate answer, most instances empty.

GPU-gated passes still record their barriers when empty, because the count is latched on the GPU and the CPU does not know it at record time.
The deferred+water stage's world steps (SSR, SAO, snow, composite, water blend), the post chain's steps and the hosted clears of those two
stages (the TAA mask and kMAIN_ONLY_ALPHA) were GPU-gated until the fixed-targets pass; they are now recorded at execution with the CPU's
answer, since every one of their hooks runs before its stage commits ("Fixed targets", below).

### The post target plan

Done by the fixed-targets pass ("Fixed targets", below): the post chain's targets no longer depend on which effects run, so there is no
plan to make, at Begin or at the end of frame. Each step is one present pass.

### The recipe check

A present pass of a CS stage that reads an aliasable CS transient (`GraphResources::IsTransientName`: Lifetime::Transient, not exported) no
present pass of its stage writes is a recipe finding (fatal in development builds). Its first run caught Terrain Blending's
`BlendedDepth16`, which is exported and so never aliased; the check counts only aliasable transients.

### No cache of built graphs

ORG holds one attached and one staged graph per epoch; a key change builds anew.

### Measurements

Passes per stage graph (the recipe's last print; before: the stage 5 build's runs `m4perf`, `m4val`; after: `k5perf`, `k6val`):

| Stage | perf-taa before | perf-taa after | validation tour before | validation tour after |
|---|---|---|---|---|
| start | 84 | 84 | 84 | 84 |
| pre-depth | 12 | 12 | 12 | 12 |
| pre-main | 21 | 21 | 25 | 25 |
| deferred+water | 87 | 74 | 85 | 68 |
| post | 222 | 78 | 218 | 78 |
| UI | 166 | 166 | 167 | 166 |

The start and UI stages are fixed instances (water effects, faces, Scaleform), present by design; their empty instances fold at execution.

perf-taa, native Vulkan, Perf-Fast, census averages between the marks, one run each, the machine in use (fps rough, no conclusion drawn):

| perf-taa | Before (`m4perf`) | After (`k5perf`) |
|---|---|---|
| Tickets a frame | 12.89 | 12.92 |
| Queue submissions a frame (batches + uploads + leading uploads) | 17.87 | 17.88 |
| Entry barriers a frame | 508.4 | 506.6 |
| Body barriers a frame, as the lists are built | 907.6 | 898.7 |
| Late barriers folded at execution a frame | (none) | 92.0 |
| Body barriers a frame, recorded | 907.6 | 806.7 |
| post stage: entry / body (as built) | 33.0 / 104.0 | 31.0 / 97.0 |
| deferred+water: entry / body (as built) | 79.0 / 165.0 | 79.0 / 163.0 |
| fps between the marks (rough) | 121.3 | 123.4 |
| Steady window: builds, recordings, switches, lagged work | 0, 0, 0, 0 | 0, 0, 0, 0 |

The post stage's absent passes were already skipped ahead and folded, so they cost few barriers before; their absence shows in the pass
counts and build sizes, not in the barriers. The recorded body barriers fall by about a hundred from the late batches' empty passes
(Scaleform's instances, the water effect instances, the faces).

The validation tour (Whiterun, inventory, map, save and load, Dragonsreach), per stage, before (`m4val`) and after (`k6val`):

| Stage | Builds | Build switches | Slot switches | Recordings | Lagging frames | Lagged work (taps) | Key builds | Key request to switch-in, frames |
|---|---|---|---|---|---|---|---|---|
| start | 13 / 13 | 13 / 13 | 13 / 13 | 52 / 52 | 0 / 0 | 0 / 0 | 0 | - |
| pre-depth | 4 / 4 | 4 / 4 | 8 / 8 | 24 / 24 | 1 / 1 | 0 / 0 | 0 | - |
| pre-main | 13 / 13 | 12 / 12 | 28 / 28 | 68 / 68 | 4 / 4 | 3 / 3 | 2 | 2, 2 |
| deferred+water | 17 / 17 | 14 / 15 | 31 / 31 | 140 / 136 | 13 / 16 | 168 / 231 | 4 | 3, 6 |
| post | 13 / 19 | 11 / 15 | 28 / 59 | 84 / 120 | 21 / 38 | 138 / 183 | 9 | 2, 3, 3, 3, 5, 5, 6 |
| UI | 5 / 5 | 5 / 5 | 8 / 8 | 48 / 48 | 0 / 0 | 0 / 0 | 0 | - |

Key builds in the tour: the first key of each stage at the world's first frame and its repeat when the chain and world clients register
there (provisional keys), the inventory's depth of field, the map's slots, the bloom radius indoors, IBL's start, and the registrations after
the load. A key change reaches the screen 2 to 6 frames after the Begin that wanted it. Frames with a stage absent after it first ran: 0
in every run.

Effect onsets: shots 4 frames after the inventory and the map open (`k4on`) against the same build with `CS_ORG_ABSENT_PASSES=0`
(`k4onoff`): the same images (inventory 83.3 both, the depth of field already drawn; the map's first frames are black in both, the engine's
own fade). The tour's inventory, map and Dragonsreach shots match the stage 5 build's by eye.

### Checks

One run each, `CS_TEST_BACKGROUND=1`, mismatches and recipe findings fatal, the final build: native-quick, native-d3d12-quick, native-river,
native-upscalers (DLSS, FSR, TAA, XeSS), native-interior, native-validation (the same 20 `VUID-vkCmdDraw-None-09600` as before), the
loading-thread run, first and third person, guest-quick: all passed, 0 tap errors, 0 recipe findings, 0 frames a stage was absent after it
first ran. The main menu (Start On Save off): menu shot correct, 0 tap errors (the harness marks it failed because it expects a world). ORG's
unit tests pass (the lease tests and HeadlessBenchmark not run; ExternalOpen failed once in a parallel run and passed alone).

The fatal path fired once during the work: the recipe check's first form counted Terrain Blending's exported transient (above).

Two description problems surfaced and were fixed: a hook's request that expired while the engine still drew the step (the VL composite,
which the description's volumetric field does not see) turned the row off every 300 frames and lagged it a few frames; a request is now kept
while the step draws. And the stages' first keys at the session's first frame (a Bink frame) had dropped the module steps nothing had
described yet; a key now changes outside the stage's frames only once the stage was described.

### What SetSkipWhen, folding and gates are still used for

- `SetSkipWhen` and the build-time fold: the slot-parity variants (TAA's and the raw AO's history pairs), passes present but off in a frame's
  description (requests that lapsed, a description that changed before its build), and every pass with `CS_ORG_ABSENT_PASSES=0`.
- The execution-time fold: late batches' passes that skip their run (instances, faces, the evaluate, gated copies and clears).
- Run gates: the per-frame choices the engine makes late (variants, instances, the screenshot copy, the tonemap's fade form, the last
  luminance level's form) and every pass's work in a run.

## Fixed post targets (recipe plan, stage 6 item; 2026-11-16, `org-native/phases`)

The post chain's steps had one pass per possible pair of targets (the tonemap 8, the depth of field 20, refraction, the HDR downsample,
TAA's output, the upsample's input), all present and gated, and `PlanEndOfFrame` picked the pair at RenderEndOfFrameEffects' entry by
replaying the engine's loop. Now each step is one pass with fixed targets, and nothing about the targets is planned.

### The design

The chain works in place on two images:
- **kMAIN** before the tonemap. It is the scene colour the Upscale anchor writes, and the VL composite already blended into it.
- **The final target** after the tonemap. It is the image the UI stage draws over and frame generation, HDR output and the present read.

A step that writes a separate target reads its image and writes that target (the HDR downsample, the luminance chain, the tonemap from
kMAIN into the final target, the screenshot copy from the final target). An effect that cannot write the image it reads draws into its
side's scratch target (`ImageSpace::HdrScratch` like kMAIN, `ImageSpace::LdrScratch` like the final target, transients), and a copy back
moves the result into the image. The copy is a plain texel copy (`AnchorPasses::Copy`), so each scratch is made with its image's size and
view format (`MakeOwnedLike`). The copy has its own gate entry; the hook of the effect's last draw that writes the scratch opens it
(`TakeChainStep`, `Part::copyBack`). The full-screen blur needs no scratch: its last draw reads only its own blurred buffer, so it draws
into the image, blended below radius 1; the engine's copy of its input into its output before that blend has no pass (`ChainDraw::Nothing`).

| Steps | Reads | Writes |
|---|---|---|
| world map, refraction, FXAA, depth of field, radial blur, get hit (before the tonemap) | kMAIN | HdrScratch, then the copy back into kMAIN |
| the same after the tonemap (depth of field, radial blur, get hit) | the final target | LdrScratch, then the copy back |
| full-screen blur (both places) | the image | the image (in place) |
| HDR copy | kMAIN | the final target |
| tonemap (both forms, both parities) | kMAIN, bloom, adapted luminance | the final target |
| TAA | the final target, history | LdrScratch and the history; the copy back |
| screenshot | the final target (after TAA, before the slots after the tonemap, as the engine copies it) | kSAVE_GAME_SCREENSHOT |
| upsample | the final target | LdrScratch; the copy back |

Why this one: a step's inputs and outputs do not depend on which other steps are present or run, so no plan exists and none can be
stale. An effect absent from the stage's build (its build pending) or one the engine did not draw this frame (its gate closed) leaves the
image as the step before it left it: the frame is whole, only without that effect, and the frame its build switches in has it. A
per-build routing (each effect writing the other of a pair, the roles resolved from the build's key) would save the copies but needs a
pass-through when a present effect does not draw, and the routing in the key; the copy costs one full-screen copy per effect drawn. TAA
pays one copy every frame it runs.

### The Tonemap anchor's targets

`RunTonemapAnchor` hands its clients a `TonemapTargets` (graph textures and view formats), not engine indices. In native mode, while the
world chain's HDR effect runs (`ImageSpace::ChainTonemapTargets`), they are CS's: kMAIN in, the final target out, the same every frame.
Elsewhere (guest mode, a menu scene's tonemap) they are the engine call's targets, imported as before.
- **Effects11** (`ORGFxRuntime::Prepare`) takes them: its shape key no longer changes when an effect moves the engine's tonemap output
  from the final target to a temporary copy. The output's size and format come from the texture handle and the anchor; the final target
  has no texture in the renderer's table in native mode, which used to leave Effects11 inactive whenever the tonemap wrote it.
- **Post Processing** (`ORGPostProcessing::BeforeTonemap`): with CS's targets it reads kMAIN and writes its result back into kMAIN only;
  it no longer writes kMAIN_COPY. In guest mode it reads kMAIN or kMAIN_COPY, whichever the input is (or refraction routed it to), and
  writes both, as before.

### The check at the end of frame

`PlanEndOfFrame` is gone. At RenderEndOfFrameEffects' entry, `CheckEndOfFrame` replays the engine's loop only to check it: the loop must
run from kMAIN into the final target, and each effect the engine has active (the slots in their place, the HDR effect or its copy, TAA,
the upsample) must be in the frame's schedule, or in the frame's own description (its build is pending: the step's hook counts lagged
work), or in the post fields read again now (the engine or CS changed it after Begin: an upscaler switch, a setting; the next
description has it). Anything else is a `NotInDescription` tap error, fatal in development builds.

Two effects are on in every description, not only in the key, because Begin cannot know them:
- refraction (slot 1): the engine turns its effect on during the frame, when it draws a refracting object. The first run of the check
  stopped there;
- the screenshot copy: a save can set its flag after Begin read it.
Both tonemap forms and both forms of the last luminance level are also on in every description now (they were on only in the key): the
engine picks one late, and the other skips at execution. This removed a lagged frame at each adaptation reset.

A key collision surfaced in the work: the full-screen blur's first draw and the depth of field's downsample had the same key (the same
effect and scope, variant 0), so the depth of field's downsample was looked up as the full-screen blur's and lagged every frame. Inner
draws now carry bit 10 (`InnerVariant`), and a registration that finds a key twice logs an error.

### CPU gates in post and deferred+water

Every hook of the post stage runs before RenderEndOfFrameEffects' return, its commit, and every hook of deferred+water (the RenderWorld
scopes, the water blend, the engine's clears there) before that stage's commit. So their passes are recorded at execution with the
CPU's gate answer (`gpuGated = false`): the chain's steps, the world steps, and the hosted clears of those two stages (the TAA mask and
kMAIN_ONLY_ALPHA clears, `Skeleton.cpp`). A step the engine did not draw skips the run and its barriers fold into the next pass that runs
(ORG's `FoldBarriersOfSkipped`). Nothing in those two stages is GPU-gated any more. GPU-gated passes remain elsewhere: the menu
composites (`MenuSteps.inl`), the main ImageSpace client and its late clients (guest and the non-stage path), and the hosted clears of the
pre-main stage and the anchors.

### Engine targets the post stage still uses

| Target | Who outside CS's chain passes reads or writes it |
|---|---|
| kMAIN | the Upscale anchor writes it (the copy of the upscaled image); Post Processing and Effects11 read it and write into it at the Tonemap anchor |
| the final target (kFRAMEBUFFER) | the UI stage draws over it; frame generation's hudless copy, HDR output and the present read it |
| kSAVE_GAME_SCREENSHOT | the save's thumbnail |
| kHDR_DOWNSAMPLE0 and the luminance levels before the last | Drawcall Limit Fix's character light may bind them |
| the VL pair, kSAO_CAMERAZ mip 10, kMOTION_VECTOR, the main depth, kTEMPORAL_AA_MASK, the normals targets, kREFRACTION_NORMALS | written by earlier stages (Volumetric Lighting, the depth upscale, forward, Deferred) and read here |

kMAIN_COPY, kIMAGESPACE_TEMP_COPY and kIMAGESPACE_TEMP_COPY2 are no longer read or written by the post stage.

### Measurements

perf-taa, native Vulkan, Perf-Fast, census averages between the marks, one run each, the machine in use (fps and thread times rough, no
conclusion drawn). Before: the absent-passes build (`k5perf`); after: `f8perf`.

| perf-taa | Before | After |
|---|---|---|
| post stage passes (the recipe's last print) | 78 | 51 |
| deferred+water passes | 74 | 68 |
| Tickets a frame | 12.92 | 12.91 |
| Queue submissions a frame (`vkQueueSubmit`: batches + uploads + leading uploads) | 17.88 | 17.88 |
| Entry barriers a frame | 506.6 | 503.2 |
| Body barriers a frame, as built / folded at execution / recorded | 898.7 / 92.0 / 806.7 | 899.3 / 93.0 / 806.3 |
| post stage: entry / body (as built) | 31.0 / 97.0 | 28.0 / 98.0 |
| deferred+water: entry / body (as built) | 79.0 / 163.0 | 78.9 / 162.8 |
| Steady window: builds, recordings, switches, lagged work, tap errors | 0, 0, 0, 0, 0 | 0, 0, 0, 0, 0 |
| fps between the marks (rough) | 123.4 | 122.4 |
| Submission thread busy, ms a frame (rough) | 2.02 | 2.34 |

The barriers barely move: the variants a frame did not use were already left out of the description and folded at the build; now they are
gone, and the ones the engine picks late skip at execution instead. TAA's copy back adds a pass and its barriers in every TAA frame.
More passes are recorded at execution on the submission thread (every step that runs, not only instances).

The validation tour (Whiterun, inventory, map, save and load, Dragonsreach), post stage, before (`k6val`) and after (`g1val`):

| | Before | After |
|---|---|---|
| Builds | 19 | 19 |
| Build switches | 15 | 16 |
| Slot switches | 59 | 39 |
| Recordings | 120 | 100 |
| Lagging frames | 38 | 24 |
| Lagged work (taps) | 183 | 139 |
| Key request to switch-in, frames | 2, 3, 3, 3, 5, 5, 6 | 2, 3, 3, 3, 3, 4, 5 |

Effect onsets: shots 1, 2 and 4 frames after the inventory and the map open (`g1on`), against the same build with
`CS_ORG_ABSENT_PASSES=0` (`g1onoff`): every frame whole and the same in both (inventory 71.9 / 71.2 / 71.2 against 71.7 / 71.7 / 71.7,
the depth of field drawn from the first frame: this save already uses it, so its passes are present). The map's first frames are black in
both, the engine's own fade. The radial blur and the world map start one frame late in both runs: Begin does not see them yet (the
check reads them again at the end of frame), the hook asks for them, and the next frame has them.

### Checks

One run each, `CS_TEST_BACKGROUND=1`, mismatches and recipe findings fatal, on the final build: native-quick, native-d3d12-quick,
native-river, native-upscalers (DLSS, FSR, TAA, XeSS), native-interior, native-validation (the same 20 `VUID-vkCmdDraw-None-09600` as
before), the loading-thread run, first and third person, guest-quick, the main menu (Start On Save off; the harness marks it failed
because it expects a world), the onset pair: 0 tap errors, 0 recipe findings, 0 frames a stage was absent after it first ran. Shots match
the previous build's by eye; Dragonsreach is a little darker in both interior shots (98.0 and 92.9 against 99.9 and 95.8), the known
variance between runs, with the same drift between the two shots. Post Processing and Effects11 were not exercised: Post Processing is
disabled at boot in this game's settings, and Effects11's preset does not own the tonemap, so the Tonemap anchor's clients ran only up to
their ownership checks. ORG is unchanged.

The fatal check fired three times during the work, each a real gap: refraction turned on during the frame (now always on), a TAA switch
between Begin and the end of frame (now read again at the end of frame), and the key collision above.

Not changed and still open: after a registration (a new size: an upscaler switch changes the dynamic-resolution ratios) the chain's
passes are not tappable until the new build switches in, so the post chain draws nothing for those one or two frames. That is older than
this change; a resize rebuilds every pass for the new size.

## A frozen world's capture frame (2026-11-16, `org-native/phases`)

Opening a menu that freezes the background (the pause menu, `Journal Menu`) used to stop the game in development builds. Its first frame
is the capture frame: Main's frame (SE 0x1405b1020) calls SE 0x1405b1650, which draws the world once with kMENUBG (target 14) as the
final target and sets the engine's captured flag (SE 0x142f26b75); the same frame then composites kMENUBG into the framebuffer (SE
0x1412e2550). The post stage's check (`CheckEndOfFrame`) treated the final target 14 as a mismatch.

**Decided at Begin.** `Driver::DecideKind` already marks the capture frame from the engine's own test (the menu stack's freeze flag, the
player's cell and 3D, CS's captured flag, reset when no menu freezes the background): `Kind::FrozenWorld` with `capture`. The check now
expects kMENUBG as the final target in that frame and the final target in every other; anything else is still a mismatch.

**The post stage writes kMENUBG.** CS's chain still ends in the final target. A new last step of the post stage, `capture.menubg`
(ChainSteps.inl, group `kCapture`), draws the final target into kMENUBG: one texel loaded per pixel (Upscaling's CopyHudlessPS) and
written in kMENUBG's format. A texel copy was tried first: the two targets have 4-byte texels of different formats, and the copied
image was garbage. The step is opened at RenderEndOfFrameEffects' return in a capture frame (`OpenCapture`).

**The engine tests the menu stack again after Begin** (Main's frame calls MenuManager's freeze test right after Renderer::Begin),
and a menu shown through the UI message queue is pushed by another thread. About one frozen-world run in three, the menu landed
between the two tests: the frame was described as a world frame while the engine drew the capture into kMENUBG, and the post
stage's check stopped the game. That call site now answers what Begin decided (`FreezeTestAtBegin`, native mode with segments);
the capture is the next frame, which Begin then sees. A log line names each frame where the two tests differed (3 of the 7 runs
after the fix).

It is **always present and gated**, not absent when unused. The engine gives no notice before the capture frame's own Begin, and that is
too late for a build (2 to 6 frames) or for a recording of a new description (a frame or more): a lagging capture frame would leave
kMENUBG unwritten for as long as the menu stays open. Every other frame skips the pass at execution, its barriers folded.

The same holds for the radial blur after the tonemap (slot 5): the pause menu starts its blur (its ImageSpace modifier) in the capture
frame itself, and the first run showed the frozen background without it while the post stage's key build lagged. Slot 5 is now on in
every description, like refraction.

**The frozen composite in the capture frame** has nothing to draw: the final target already holds the image it would copy back. Its
draw is withheld (MenuSteps.inl). In the frozen kind's later frames it is the menu UI stage's first step, as before.

**DCLF's load hold no longer holds the presents for a staged build.** The capture frame asks for a post stage build (the radial blur's
key, before the change above), and DCLF's re-entry hold (`ShadowCapture`, `OcclusionReady`, view coverage) started on any rebuild
request: it held the presents through the whole menu, so the menu was never shown. They now ask
`RenderGraphRuntime::RebuildRequestedUnstaged` (`BuildManager::UnstagedRebuildRequested`): a staged CS stage keeps running its attached
graph until its build switches in whole, so its request leaves nothing undrawn. In the native-quick runs this also ended the back-to-back
5-second re-entry holds after the load (6 in the previous build's run, 1 now).

### Measurements

The frozen kind (pause menu over the world, `h3fz`, a census window of 129 frozen frames and 34 world frames): the menu UI stage is the
frozen frame's one ticket (1.0 a frame: the frozen composite, two display segments, the present's clients, the overlay, by the frame
sequence census), about 40 entry and 200 body barriers as built a frame. The capture frame is a world frame (the world recipe) plus the
gated copy. No clear runs on its own anchor's ticket in frozen frames (a new log line names any, with the frame kind, a few after each
kind switch).

perf-taa, native Vulkan, Perf-Fast, one run each, the machine in use (fps rough):

| perf-taa | Before (`e2perf`) | After (`i1perf`) |
|---|---|---|
| Tickets a frame | 12.8 to 13.0 | 12.8 to 13.0 |
| Queue submissions a frame (batches + uploads + leading uploads) | 14.7 + 2.0 + 1.0 | 14.7 + 2.0 + 1.0 |
| Entry / body barriers a frame (body less folded) | 496 to 510 / 796 to 814 | 501 to 508 / 803 to 812 |
| fps (rough) | 123.4 | 124.8 |

### Checks

One run each, `CS_TEST_BACKGROUND=1`, mismatches fatal: the frozen-world run native (`Journal Menu=show`, shot, hide, shot) and guest,
native-quick, native-d3d12-quick, native-river, native-upscalers, native-interior, native-validation (the known 20
`VUID-vkCmdDraw-None-09600`), the loading-thread run, first and third person, guest-quick, native-perf-taa: all passed, 0 tap errors, 0
recipe findings, 0 frames a stage was absent after it first ran. The main menu (Start On Save off): menu shot as before (the emblem still
missing); the harness marks it failed because it expects a world. The pause menu shot native against guest: the same menu and the same
blurred frozen background. After the menu closes the world resumes (world frames, the stages current, 0 tap errors).

## The sun stage, and the capture in the UI stages (2026-11-16, `org-native/phases`)

### The sun's queries: a sun stage and the post stage's first step

`Sun::SubmitOcclusionQuery` (SE 0x1403bbf20) has two callers: `Main::RenderWorld` (after Drawcall Limit Fix's forward epoch) and
`Main::RenderFirstPersonView` (after the first-person epoch). Each was the EngineQueries anchor's own ticket (1.9 a frame).

- **The world's query: the sun stage** (`SegmentId::Sun`, `Segment::DeclaredSun`, between deferred+water and post). The query must run on
  the GPU before the first-person geometry, which would occlude the sun, and after DCLF's forward epoch; nothing of CS's runs between
  those two DCLF graphs. So it is a stage of its own: one step (the sun's client at the EngineQueries anchor, a shared step naming
  `cs.sky.sun`) and a point end. It commits just before DCLF's first-person epoch is submitted (FlushRegions, `EndsSun`), or before the
  post stage's work in a frame without that epoch, or at a later stage's first tap. `CS_ORG_SEGMENT_SUN=0` turns it off.
- **First person's query: the post stage's first step.** It comes after the first-person epoch, when the post stage's window is open
  (the sun stage has ended), so it costs no ticket.
- **Instances.** The sun's client has two instances in a stage run (`kSunInstances`): a stage run's k-th tap is instance k, with its own
  gate entries, latch region, run and query. In a frame without the first-person epoch both queries fall in the sun stage (instances 0
  and 1). The client takes instances only when a stage names it (`FrameAnchors::NamedByStage`).
- **The query ring in a stage** (`OcclusionQueries.cpp`). A stage's taps commit into the frame slot's staging run, and the stage's commit
  moves them onto the run that submits it. `QueryRing` now moves its runs the same way (a `RunSlotsRegistry` entry: the staging lease's
  range, its engine slots and its generation move to the submitting run's lease). A second commit of one run adds its queries
  (`Commit`'s first query), and the passes reset and resolve their instance's part of the range. A committed run that is not submitted
  yet reads not-ready (the bracket no longer skips it at its end); a run whose lease comes round without a submission is skipped then.
- The anchor's own epoch keeps a query no stage's window takes: a world frame before Drawcall Limit Fix draws (deferred+water is still
  open), seen in the loading frames.

The frame sequence census of a steady world frame: forward, the world's query (held), `submit CS stage: sun`, `submit CS DCLF: first
person`, first person's query (held), the post stage's taps. The query answers are as before (the same counts at the same spot, a few
more not-ready polls, since the answer comes after the stage's later commit).

**The decal tests are not tickets in these sessions.** The decal occlusion client is never made: `kDECAL_OCCLUSION has no depth view to
read its format and size from` at its setup, in every run; the DecalQueries anchor ran 0 times a frame in every run. Moving them into the
start stage (fixed instances, a query range per instance, which the ring's per-instance ranges above now allow) is not done: there is
nothing to test it on.

perf-taa, native Vulkan, Perf-Fast, one run each (before: `i1perf`, the capture-frame build; after: `k2perf`, with the sun stage and the
capture pass), the machine in use (fps rough):

| perf-taa | Before | After |
|---|---|---|
| Tickets a frame | 12.8 to 13.0 | 11.8 to 12.0 |
| Queue submissions a frame (batches + uploads + leading uploads) | 14.7 + 2.0 + 1.0 | 14.0 + 2.0 + 1.0 |
| Entry barriers a frame | 501 to 508 | 494 to 503 |
| Body barriers a frame, recorded (less those folded at execution) | 803 to 812 | 806 to 819 |
| engine queries anchor: tickets, entry / body | 1.96, 12.7 / 9.8 | none |
| sun stage: tickets, entry / body | - | 0.88 to 1.00, 5.3 to 6.0 / 4.4 to 5.0 |
| fps (rough) | 124.8 | 122.6 |

The world frame's tickets: start, shadow view, pre-depth, Z-prepass, pre-main, main opaque, deferred+water, forward, sun, first person,
post, UI.

Checks (one run each, `CS_TEST_BACKGROUND=1`, mismatches fatal; the build with the capture pass too): native-quick (with the frame sequence
census), the frozen-world run native and guest, native-d3d12-quick, native-river, native-upscalers, native-interior, native-validation
(the known 20 `VUID-vkCmdDraw-None-09600`), the loading-thread run, first and third person, guest-quick, native-perf-taa, the main menu:
all passed (the main menu as before), 0 tap errors, 0 recipe findings, 0 frames a stage was absent after it first ran. The validation
tour failed once to load within 120 s (`k2val`: 794 re-registrations of the shadow clears, each a DCLF re-entry hold, see below) and passed
on the next run (84); the switch registry did not list `CS_ORG_SEGMENT_SUN` then, and does now.

### The capture: a pass of the UI and menu UI stages

The harness's shots were D3D11 copies in native mode: `capture N was not in the epoch submitted at its capture point; D3D11 copies it`.
The capture's pass made its readback plan in its preparation, and a kept recording is not prepared again, so the Capture anchor's ticket
held no copy.

Now (`ORGScreenshot.cpp`):
- The Capture anchor is the last step of the UI and menu UI stages, after the overlay (wholly held: no epoch of its own in native mode).
- Native mode's capture point moves to just before those stages' commit at the present (`Hooks.cpp`, before the stages' `TapPoint`); the
  capture point inside HDR Display's present chain still runs, and takes no copy then.
- The pass is recorded at execution from the capture its run's commit stored (`RunSlots`, a frame input). A run without a capture skips
  it, its barriers folded.
- The readback buffers are CS's (two, sized for the source, row pitch aligned to 256), declared as copy destinations at the build. The
  stage's executed callback takes the run's GPU point; a later capture point maps the buffer once the point is reached and hands the bytes
  to the encoder.
- A capture that finds no run holding its copy (the first capture names a source, and the build with it switches in a few frames later)
  waits and is taken at a later capture point; the first capture of a session waits about 6 frames.
- The D3D11 staging copy is used only when the graph cannot take a capture at all: inactive, a source it cannot import, or both readback
  buffers in flight. No run fell back.

Frame generation's hudless copy (the FrameGeneration anchor) is not a ticket in the test runs: frame generation is off.
Captures in the checks: every harness shot of the runs above came through the graph (`capture N copied on the render graph`, read back 3
capture points later; the first of a session waits for its source's build, 9 to 13 capture points), including the main menu's (the menu
UI stage) and the pause menu's (the frozen kind). The UI stage gains one pass (the copy), which skips at execution in frames without a
capture.

### The main menu's emblem: found, not fixed

*Fixed in part by "Cleanup pass", item 2 (below): the shadow casters were not the cause.*

The emblem (the main menu's 3D logo) is missing in native, and since the menu scenes' frame inputs the mist is missing too (both are drawn
in guest; earlier native shots, `d1mm`, show the mist and the emblem as a black silhouette over it). What the runs show:
- The menu scene's colour batch is latched whole every main-menu frame: one Lighting draw (8991 indices, three targets, the emblem) and
  one Effect draw (600 indices, the mist). The scene's clears go to the menu UI stage's places as expected (kMAIN at the start and scene
  places, the shadow map slice and the main depth at the scene place, the main depth again after the composite). The composite is the
  HDR effect from kMAIN into the framebuffer (`UI3DSceneManager::Render`'s end-of-frame call).
- One engine draw a main-menu frame reaches D3D11 with nobody to draw it and is skipped (the flush latch's line, now with the caller chain
  further up and the draw's technique): a Utility depth-only draw into depth target 4 (the shadow maps), from AE 52729, which
  `UI3DSceneManager::Render` (AE 52727 + 0x2b2) calls to render the menu scene's shadow-casting lights (their vtable + 0x48 and + 0x50,
  down to BSBatchRenderer). So the scene's shadow map is never drawn in native mode, and the emblem is shaded against a shadow map that
  holds only the scene place's clear.
- The main menu frame has no depth batch and no shadow mask run (`UI3DSceneManager::Render`'s depth StartAccumulating block does not run
  for this scheme), unlike the loading screen's.
Not done: drawing the menu scene's shadow casters (a client for AE 52729's shadow light render, like the depth batch's), and finding why the
mist's Effect draw shows nothing since the frame-input change. Both need a frame capture to confirm, which the background runs cannot
take.

### Not done in this pass

*Updated by "Cleanup pass" and "State of the native frame" (below).*

- **HDR output in the UI stage.** No harness setting or switch enables HDR without an HDR display (only the user's settings file, which the
  runs must not change), so it could only be built, not tested, in the present path; and the back buffer copy binds the swap chain's image
  of the frame, which a kept recording cannot bind. Left on the HDROutput anchor's own ticket (HDR is off in every test configuration). With
  HDR on, the capture now comes before HDR output (the capture point moved to the UI stages' commit); a capture of HDR's composite then
  copies the previous frame's.
- **A menu's scene over the live world** (inventory item model, local map): still the EngineDraws and ImageSpace anchors' own epochs.
- **Deleting per-anchor epochs and `CanRun`/`RequestJoin`**: not started; the list below is what is still reachable.
- **The shadow clears' churn** (older than this pass): depth target 10 (the precipitation occlusion map) is a new texture again and again
  (84 to 794 times a run), each time the shadow clear group registers again and its anchor's graph is rebuilt (not staged), which starts a
  DCLF re-entry hold. The validation tour's load once ran out of its 120 s under 19 such holds.
- **DCLF's first load hold** ends with "no progress" after about 7.5 s plus one 5 s re-entry hold, in every native run (older than this
  pass).

### What is still a ticket at a hook, by frame kind (2026-11-16)

*Updated by "Cleanup pass" and "State of the native frame" (below).*

- **World**: none in a steady frame but the stages and Drawcall Limit Fix's graphs (start, shadow view, pre-depth, Z-prepass, pre-main,
  main opaque, deferred+water, forward, sun, first person, post, UI). Outside steady frames: the sun's query and the shadow and frame
  clears in world frames before Drawcall Limit Fix draws and during its load holds (the frame clear about one a frame then); a menu's scene
  over the live world (EngineDraws, ImageSpace); blood and precipitation (EngineDraws); HDR output when on; frame generation's hudless copy
  when on (not exercised).
- **Menu over a frozen world**: none; the menu UI stage is the frame's one ticket. The capture frame is a world frame.
- **Main menu, loading screen**: none (the menu UI stage), but the main depth clear of frame 0, before the first Begin.
- **Bink movie**: none (the UI stage).

### What of the old native paths is still reachable (2026-11-16)

*Updated by "Cleanup pass" and "State of the native frame" (below).*

- Per-anchor epochs: EngineDraws (blood, precipitation, a menu scene over the live world), ImageSpace (the menu composites in a world frame,
  the face tint at a load), EngineShadowClear and EngineFrameClear (world frames before Drawcall Limit Fix draws, its load holds, frame 0,
  views no place clears), EngineCubeClear (outside world frames), EngineCopy outside the stages' windows, EngineQueries (world frames before
  Drawcall Limit Fix draws), DecalQueries (registered only when its client is made: not in these sessions), HDROutput (HDR on), Tonemap and
  FrameGeneration (a menu scene's tonemap, frame generation), MovieTiles (no client). Capture: none in native mode with the UI stages.
- `CanRun`/`RequestJoin`: the sky clients' and the decal client's setup joins, the menu composites in a world frame (`MenuClientDraws`), the
  skeleton's group clears (`DrawFor`), the upscaler evaluate's first frames (`FrameAnchors::CanRun` at the Upscale commit).
- Guest mode keeps every anchor's own epoch, the main ImageSpace client and its manifest, and the D3D11 renderer for displays the graph
  refuses.

## Cleanup pass (recipe plan, stage 7 without the final measurement; 2026-11-16, `org-native/phases`)

The plan's stage 7 asked for the old native paths to be removed, and for six items before that. What was done, item by item.

### 1. The shadow clears' churn and the first load hold

**The churn was CS's.** Skylighting's `RenderSkyOcclusionMap` puts its sky occlusion map into the precipitation occlusion map's slot of
the engine's depth table (depth target 10) while the engine's `Precipitation::RenderMask` renders into it, and puts the engine's map back
after. A clear in that window (RenderMask's, or Skylighting's own D3D11 clear) found another texture in the slot, and the skeleton took
the target for made again: the shadow clear group registered again (84 to 794 times a run). Each registration rebuilt the shadow clear
anchor, Drawcall Limit Fix's shadow-view graph (its hosted clears, `extension 'cs.engine.skeleton.hosted' added again`, 161 times in one
hold) and the menu UI stage, none of them staged. `RebuildRequestedUnstaged` was true nearly every frame, so DCLF's hold counted a held
view in most frames ("a view not covered 263 frames") and ended with "no progress"; its re-entry hold followed for the same reason.

**The fix** (Engine/Skeleton): a clear whose view is not the one its pass was made for stays on D3D11 for that call (counted, logged
once), and a target made again is checked at the frame's start (`CheckMadeAgain`, from `BeginFrame`), when a lent slot holds its own view
again. A real resize still registers the group again, once.

**The first load hold** waited for that churn and nothing else: with it gone the hold settles. One short re-entry hold remains after the
load (0.4 to 0.9 s, settled): a stage's one-shot passes ran, and `Rebuild` asks for every epoch of the stage's anchors, the per-anchor
epochs of its shared anchors (engine depth copy, engine draws, image space, engine copy) among them, which are not staged. It goes with
those epochs (item 5, not done).

| Run (measured) | Before (`k2q`, `k2val`, `k3val`) | After (`a1q`, `a1val`) |
|---|---|---|
| shadow clear re-registrations (quick / validation tour) | 170 / 794 and 84 | 0 / 0 |
| DCLF holds started (quick / validation tour) | 3 / 21 and 10 | 3 / 10 |
| holds ended with "no progress" (quick / validation tour) | 2 / 19 and 2 | 0 / 0 |
| first load hold, then its re-entry (quick) | 7.5 s, then 5.6 s, both "no progress" | 3.1 s, then 0.37 s, both settled |
| first load hold, then its re-entry (validation tour) | 9.6 s and 9.5 s, then 6.1 s | 4.7 s, then 0.9 s, settled |
| load start to the world on screen (quick: the loading screen's start to the end of the last hold of the first load) | 18.6 s | 10.0 s |

**The capture fix found on the way.** The graph capture (the last pass of the UI stages, "The capture: a pass of the UI and menu UI
stages") took its GPU point in its executed callback, which a stage runs at the tap, before the stage's commit submits the copy. Poll then
mapped the readback before the copy ran: the main-menu shot was all black and the pause menu's shot repeated the previous capture. The
point is now taken at the next capture point (`ORGScreenshot::Poll`), which always follows the commit of the run that took the capture.
Every native shot of the earlier sections' last pass (`k2*`) was affected in light frames (menus); world shots were mostly right by timing.

### 2. The main menu's emblem and mist

**The mist.** A CS stage uploads CS's shared constants (FrameBuffer, SharedData, FeatureData, Permutation) once per step that reads them,
at the step's upload point (`SharedConstants::MakeUploadPass(k)`), and there were 8 points for up to 16 steps. The menu UI stage's scene
colour (step 8) and its composite (step 9) had no upload and no commit: they read what an earlier step's upload left, zeros from a step
that did not tap this frame. There is now a point per step (`kUploadPoints = 16`, a static assert against `kMaxSteps`).

**The emblem** was black with the menu scenes family on both backends (guest with `CS_ORG_MENU3D=1 CS_ORG_ENGINE_CONSTANTS=1` shows the
same), so the shadow-caster draw skipped in native was not its cause: guest draws that shadow map with D3D11 and its emblem is black too.
The latched pixel constants showed why: DirLightColor, DirectionalAmbient and the point lights' rows are zero while the light count is 2.
Light Limit Fix replaces the engine's point light setup (its SetupGeometry hook): the engine leaves those rows empty and CS's D3D11 shader
reads LLF's strict lights. The scene build has no Light Limit Fix. The strict lights of the pass are now written into the rows the build
reads (`LatchPointLights`, MenuScenesCapture.cpp): position relative to the eye with the radius, colour with the fade.

The emblem shows, lit by its two lights, but darker and flatter than guest's: the scene build falls off as the vanilla shader does
(1 - (d/r)^2), not as Light Limit Fix's shader does. Matching it needs the scene build to take LLF's strict-light path (its constants as a
latched block): not done.

**The menu scene's shadow casters** (AE 52729, a Utility depth draw into the shadow maps from `UI3DSceneManager::Render`) are still
skipped in native. The main menu's scene has no shadow mask run, so nothing there reads that shadow map; the loading screen's models read
it through the shadow mask. A client for that draw (a menu scene batch with the shadow map slice as its depth target) is not built.

**`CS_ORG_SHOT_TARGET=<n>`** (new, diagnostic): a harness shot copies engine render target n instead of the framebuffer. It found the
fault above (kMAIN empty in native, the emblem's geometry present in the TAA mask, target 4).

Shots (main menu, Start On Save off; guest is the reference): native before showed only the menus; now the mist and the emblem (darker).

### 3. A menu's scene over the live world, blood and precipitation: not done

What the tests reach of it: the validation tour's inventory draws no item model (`menu 3D scheme 1 (inventory): 0 geometries`), and no
blood or precipitation ran in any test run; an item model needs a selected item, which needs foreground input. What the inventory does
reach is one frame clear a frame on the EngineFrameClear anchor's own ticket while the menu is open, inside the UI stage's window (counted
as "GPU work submitted between a stage's first tap and its commit", 11 to 47 a census window during the inventory). Moving the scene into
the UI stage needs the menu UI stage's layout there (displays before and after the scene, the clear places in world frames, 16 Scaleform
instances), which could not be checked by any background run, so it was not started.

### 4. TAA without its copy back

`ChainRoute` (ChainSteps.inl). A post stage build whose key has TAA, the HDR effect and CS's tonemap is routed: the tonemap draws into
the LDR scratch, TAA draws from there into the final target, and nothing copies. Otherwise the direct forms (TAA from the final target into
the scratch, then the copy back) are in the build.

- **One routing per build.** The key's description picks the routing from the frame's fields; the routed and direct passes are rows of
  their own (the tonemap's variant bit 14, TAA's bit 13, `ChainRoute::kRouted/kDirect`).
- **A frame describes its build's routing.** `FrameAnchors::DescribedBuildHasRow` (new) answers, inside a client's rows callback, whether
  the build the frame is described against has a row. So a frame whose wish differs from its build (TAA just turned on or off) is not a
  lagging frame: it runs the build's routing whole. While a build holds both (the key keeps the other routing for its use window), the
  frame's own wish decides.
- **The frame stays whole while a build is pending.** In a routed build's frame without TAA, a pass-through copy (`taa.through`) moves the
  scratch into the final target at RenderEndOfFrameEffects' return. A frame whose tonemap did not write the scratch (HDR off, Effects11's
  tonemap) draws no TAA (lagged work); its tonemap or copy wrote the final target.
- The hook takes the routed tonemap and TAA when the frame's schedule holds them (its rows).

Other effects decided by the description that always draw when present: the dynamic-resolution upsample (it still copies back). It sits
after the slots after the tonemap, so routing it would route those slots too; not done.

Measured (perf-taa, before `k2perf`, after `d1perf`): the post stage's build now holds the routed tonemaps, routed TAA and the pass-through
copy, and no `taa.copyback`; the TAA frame draws one full-screen copy fewer. Entry / body barriers of the post stage are unchanged as
built (30 / 113: the copy's place is taken by the pass-through copy, which skips and folds), recorded body barriers unchanged within noise.
native-upscalers (DLSS, FSR, TAA, XeSS: routing switches both ways in one run) passed with 0 tap errors; its TAA shot matches the previous
build's.

### 5. Deletions

Done:
- The A/B switches whose old side is no longer wanted, and their code: `CS_ORG_SEGMENTS` (`=0` off, `=3` closed), `CS_ORG_SEGMENT_REST`
  (the closed segments' rest-at-last-use), `CS_ORG_SEGMENT_POOLS=0`, `CS_ORG_SEGMENT_START`, `_PREMAIN`, `_DEFERRED`, `_SUN`, `_POST`
  (also the post chain's hooks), `CS_ORG_UI_STAGE`, `CS_ORG_STAGED_BUILDS`, `CS_ORG_SEGMENT_CLEARS`, and the stale `CS_ORG_SEGMENT_WORLD`
  registry entry. Native mode with its requirements met always runs every CS stage, staged, open, with private alias pools. Kept for the
  final measurement: `CS_ORG_ABSENT_PASSES` and `CS_ORG_KEEP_SKIPPED_BARRIERS`. ORG's `SetClosedEpochs` and `SetRestAtLastUseEpochs` stay in
  ORG (a library feature, unit-tested there); CS no longer calls them.

Not done, and why:
- **Per-anchor epochs** (EngineDraws, ImageSpace, the shadow, frame and cube clears, EngineCopy, EngineQueries, DecalQueries, MovieTiles,
  Tonemap, FrameGeneration) and `CanRun`/`RequestJoin` for their clients: still reached in native (below, "What of the old native paths is
  still reachable"), and guest mode runs every anchor on its own epoch. They go when item 3 and the clears before DCLF draws are stages.
- **Hosted clears, shared and point steps, `repeats`, held tickets from many taps**: all still used by the stages (DCLF's shadow-view and
  Z-prepass hosts, the shared ImageSpace/EngineDraws/EngineCopy/Tonemap steps, every stage's end point, the menu composites and water
  effects that tap more than once).
- **D3D11 replay of clears and copies** (`Skeleton::Replay`, `RunCopy`'s D3D11 path): reached when a group anchor's run does not submit
  (an error), and in guest mode.
- **The phases and regions code** (`RegionsOn`, `CS_ORG_PHASES=1`, the `Segment::Phase*` epochs, held phase executions in `Run`): off by
  default and unreachable without that switch, but woven through `Run`, the snapshot publisher, graph gathering and the runtime's epoch enum;
  removing it safely needed more build cycles than this pass had. Left as it was.
- `AllowedBetween` was already gone (stage 2).

### 6. Expected warnings

- **`PublishedResources used on thread …, not the render owner`** [C]: a graph build on the build manager's thread asked `IsPublished` /
  `IsRegistered`, which read the live entries the render thread changes: a real race. They now answer from the snapshot the graph being
  built registers, as `IsDeclarable` already did. That race is closed, but the line still comes once a run: the same build thread reads
  the live entries again during the build's bootstrap (after its passes are gathered, before the persistent graph is prepared), most
  likely a pass's Prepare calling `ShaderResourceIndex`, which needs an entry's current version and published flag that the build's
  snapshot does not hold. Not fixed: the snapshot would have to carry them.
- **16 "submitted between the first tap and the commit of the UI stage" at frame 1**: the first frame's initial layouts and zero fills of
  the engine targets (external work, `initial layout Engine::kMAIN`, `zero Engine::kMAIN`, ...), made on first use, inside the UI stage's
  window because the stage is present from frame 1 (its first schedule is recorded at setup). Harmless: they initialise images nothing has
  written, before the stage's commit submits any work that reads them; the order between them and the stage's work cannot matter. Not
  changed.

### Measurements and checks (final build, DLL c60786c7b244)

perf-taa, native Vulkan, Perf-Fast, census windows of 50 world frames, one run each; before `k2perf` (the sun stage and capture build),
after `e1perf`. The machine was in use: fps is rough and no conclusion is drawn from it.

| perf-taa | Before | After |
|---|---|---|
| Tickets a frame | 11.8 to 12.1 | 11.8 to 12.0 |
| `vkQueueSubmit` a frame (batches) | 13.7 to 14.1 | 13.6 to 14.0 |
| Entry / body barriers a frame, as built | 494 to 503 / 902 to 916 | 492 to 505 / 902 to 917 |
| Late barriers folded at execution / body recorded | 97.1 / 818.9 | 98.0 / 815.1 |
| Post stage passes (the recipe's last print) | 61 | 71 (both TAA routings' rows are registered; the build holds the routed ones) |
| Steady window: builds, recordings, switches, lagged work | 0, 0, 0, 0 | 0, 0, 0, 0 |
| Loading screen to world-ready (harness) | 21.6 s | 11.9 s |
| fps (rough) | 122.6 | 121.8 |

The world frame's counts do not move: the churn and the holds were load-time costs, and TAA's copy was one pass of about a hundred
recorded in the post stage.

Checks, one run each, `CS_TEST_BACKGROUND=1`, mismatches and recipe findings fatal: native-quick, native-d3d12-quick, native-river,
native-upscalers, native-interior, native-validation (the same 21 validation lines as before), the loading-thread run, first and third
person, the frozen-world run native and guest (the pause menu shot now differs from the world shot before it, and matches guest's), guest-quick,
native-perf-taa: all passed, 0 tap errors, no `NoPass`, 0 recipe findings, 0 frames a stage was absent after it first ran, Scaleform 0
displays left out, 0 shadow clear re-registrations, 0 holds ended with "no progress". The main menu native and guest (Start On Save off;
the harness marks them failed because it expects a world): native shows the mist and the emblem, darker than guest's.
ORG is unchanged in this pass.

## State of the native frame (2026-11-16, after the cleanup pass and the render-thread pass)

Native mode (`CS_ORG_NATIVE=1`), Vulkan or D3D12. Every CS stage is one ORG graph, built off the render thread, staged and recorded before
it switches in at a frame's `Renderer::Begin`, one kept recording per frame slot, one ticket a frame. Drawcall Limit Fix's graphs (*) are
stages of their own.

### Stages per frame kind

| Kind | Stages, in order |
|---|---|
| world, and a frozen world's capture frame | start, shadows*, pre-depth, depth*, pre-main, opaque*, deferred+water, forward*, sun, first person*, post, UI |
| loading screen, main menu, menu over a frozen world | menu UI |
| Bink movie | UI |

A steady world frame is 11.8 to 12.0 tickets: the stages' and DCLF's (the sun stage takes 0.9 to 1.0; a frame without the first-person epoch
puts both sun queries in it).

### Always present, chosen per frame by gates

Present in every build of their stage, recorded at execution with the CPU's gate answers (a pass that does not run skips, its barriers
folded), because the engine decides them late or a lagging build would lose them:
- start: the six reflection faces' sky, the water effects' instances (8 ripples and 4 heightmap steps per target, 32 flow copies);
- post: the screenshot copy, the frozen background's copy (`capture.menubg`), refraction and the radial blur after the tonemap, both tonemap
  forms, both forms of the last luminance level, the engine-counter parity variants (luminance, bright pass, the VL composite, the
  normals target), TAA's and the raw AO's history pairs (by slot parity, in the description), and in a routed build the pass-through copy;
- deferred+water: the snow pass and its copy, the world steps' variants by the engine's counters;
- UI and menu UI: Scaleform's instances (8 in UI; 16 in menu UI), the capture's copy, the movie's tiles, the menu UI stage's three clear
  places;
- sun and post: the sun's two query instances.

Absent by build key when off (a rebuild, staged, on change): the post slot effects, the HDR effect against its copy, the Effects11 tonemap
owner, TAA (and its routing), the upscaler evaluate, the dynamic-resolution upsample, the VL composite, SSR, the SAO composite forms, IBL,
Terrain Blending, Volumetric Shadows, light culling, the bloom radii.

### Engine targets still used, and who reads them

| Target | Writers | Readers |
|---|---|---|
| kMAIN | DCLF's opaque and forward graphs, deferred+water, the Upscale anchor's copy, Post Processing and Effects11 at the Tonemap anchor, the menu scenes | the post chain (in place before the tonemap), the menu composites, Scaleform (a menu drawn into kMAIN) |
| kFRAMEBUFFER (the final target) | the post chain (tonemap or routed TAA, then in place), the menu composites, Scaleform, the overlay | the UI stages, the capture, HDR output, frame generation's hudless copy, the present |
| main depth, kPOST_ZPREPASS_COPY | DCLF's Z-prepass, the pre-main and menu UI depth copies | deferred+water, post, the menu scenes' soft depth |
| the normals targets, kMOTION_VECTOR, kTEMPORAL_AA_MASK, kSHADOW_MASK | DCLF's graphs, pre-main's clears, the menu scenes | deferred+water, post (SSR, SAO, TAA), the menu scenes' Lighting draws |
| kMAIN_ONLY_ALPHA, kMAIN_COPY, kREFRACTION_NORMALS, kUNDERWATER_MASK, the water displacement pair and flow map | forward, deferred+water, the start stage | the next frame's start and deferred+water (`kPreviousFrame`) |
| kHDR_DOWNSAMPLE0 and the luminance levels before the last | post, the menu composites | DCLF's character light |
| the shadow maps, the precipitation occlusion map | DCLF's shadow view graph (and the engine at its occlusion point while DCLF does not draw a map) | DCLF's graphs, Skylighting (its own map lent into the slot), the precipitation |
| kMENUBG | post's capture copy in a capture frame | the menu UI stage's frozen composite |
| kSAVE_GAME_SCREENSHOT | post's screenshot copy | the save's thumbnail |
| the reflection cubemap faces | the start stage | DCLF's graphs |

### What guest mode (DXVK) still uses of the old path

Guest runs no CS stage. It uses: every frame anchor's own epoch (per-anchor epochs, `CanRun`/`RequestJoin`), the main ImageSpace client,
the clients built on first sight and the shape manifest (`ImageSpaceShapes.json`) with the per-draw shape capture, the D3D11 renderer for
displays the graph refuses, the capture's D3D11 staging copy when the graph cannot take it, and the post
chain's hooks (which only check and stage in native). This is the input for the guest port (plan stage 8).

### Old native paths still reachable

- Per-anchor epochs: EngineDraws (a menu's scene over the live world, blood, precipitation), ImageSpace (the menu composites over the live
  world, the face tint at a load), EngineShadowClear and EngineFrameClear (world frames before DCLF draws and during its holds, a menu scene's
  clears over the live world, frame 0), EngineCubeClear (outside world frames), EngineCopy outside the stages' windows, EngineDepthCopy
  outside the stages, EngineQueries (world frames before DCLF draws), DecalQueries (its client is never made in these sessions), HDROutput
  (HDR on), Tonemap and FrameGeneration (a menu scene's tonemap, frame generation), MovieTiles (no client). A stage's one-shot passes now
  rebuild only the anchors they sit at (the ImageSpace epoch for the histogram reset), so the re-entry hold after each load is gone
  ("Render-thread cost").
- `CanRun`/`RequestJoin`: the sky and decal clients' setup joins, the menu composites over the live world, the skeleton's group clears, the
  upscaler evaluate's first frames, the menu scenes family.
- Clears of a lent target slot (Skylighting's map) stay on D3D11: the skeleton does not take them. A skeleton clear or copy run that does
  not execute is now a tap error (`NotExecuted`, the mismatch policy); nothing replays it on D3D11 (Cleanup A1).
- The phases and regions code is removed (`CS_ORG_PHASES` with it). `FrameAnchors::FlushRegions` kept its other jobs, the stages'
  commit points and the open-stage census, as `BeforeGpuWork`. `PassDesc::gated` is removed (Cleanup A1).

### Untested

- HDR output (no background way to enable it), frame generation (needs the foreground), Post Processing and Effects11 at the Tonemap
  anchor (Post Processing is off in this game's settings; Effects11's preset does not own the tonemap), so the routed TAA's interplay with
  Effects11's tonemap is untested too.
- A menu's item model over the live world, blood, precipitation (no test reaches them).
- The menu scene's shadow casters (not built), the menu scenes' Light Limit Fix falloff (not matched).
- The effects that are `NoPass` (not built): the rain ripple, rain heightmap, blend, smooth and normals water steps, ISSimpleColor's stencil
  fill and the UI form of TAA in a menu scene, other slot effects inside a menu scene's end-of-frame call, the local map's edge pass, the
  console screenshot's and the debug views' copies, the lens flare composite forms, the masked depth of field composites, the raster
  depth of field blur.

## Per-thread cost (2026-11-16, `org-native/phases`)

The first part (to "Left on the render thread") is the render-thread pass; the parts after it are the per-thread pass: the frame cap,
the uncapped comparison, the attribution by thread and what was cut.

The timing pass found native perf-taa CPU-bound on the render thread: steady frame 7.5 ms against guest's 6.95 ms, with about 4.0 ms of
render-thread CPU outside any Tracy zone (guest: about 1.4 ms), and a hitch every 120 frames.

### The hitch

Drawcall Limit Fix's `ThreadBusy::Sample` looked for DXVK's `dxvk-cs` thread every 120 frames until it found it, by walking every thread
in the process: 60 to 80 ms on the render thread each time. Native mode has no DXVK, so the search never ended. Native mode no longer
searches; guest mode searches on a helper thread (every 2 s, up to 30 tries) and the render thread only reads the result.
perf-taa native: 6 frames over 20 ms in each 750-frame window before, none after.

### Two diagnostics

- `CS_ORG_D3D11_METER=1` meters every tripwire handle method on the thread that ends frames: calls and exclusive time per method,
  summed into families. Tracy plots `CS.D3D11.<family>.calls` and `.us`; the log has the families and the 20 largest methods every 600
  frames, with the thread's CPU time (TripwireMeter.cpp).
- `CS_ORG_RT_SAMPLE=<seconds>` is a sampling profile of the render thread inside the process, without ETW or elevation (neither Tracy's
  sampling nor xperf works here: they need an elevated session). A helper thread suspends the render thread about once a millisecond,
  unwinds its stack and resumes it; the test step `mark start` begins it. The log has leaf and inclusive functions, and for each system leaf
  its first CS or game caller, with the share of samples and ms a frame (RtSampler.cpp). A sample counts when the thread used CPU since
  the last one, so a thread that just went to wait counts too: the engine's `WaitForFrameJobs` shows up in both modes (about 1.5 ms a
  frame); it is not CPU time.

### Attribution (perf-taa, native Vulkan, measured, one run each)

The guess was the tripwire handles. They are cheap:

| Family (tripwire handles, render thread) | Calls a frame | ms a frame |
|---|---|---|
| state sets (IA/VS/PS/CS/OM/RS Set*, ClearState) | 1,056 | 0.032 |
| state gets | 62 | 0.003 |
| maps and updates (Map 245, Unmap 245) | 490 | 0.078 |
| COM and private data (GetPrivateData 100) | 120 | 0.005 |
| clears and copies | 1 | 0.006 |
| draws and dispatches | 1 | 0.003 |
| swap chain, creates, queries | 2 | 0.001 |
| total | 1,730 | 0.13 of 6.0 ms thread CPU |

The sampling profile put the cost elsewhere (share of on-CPU samples, ms a frame scaled by the thread's 6.0 ms; one 5 s run):

| Where | ms a frame | What |
|---|---|---|
| `DCLF::Family::CopyConstants` from the water and sky families' `TryLatchDraw` | 1.45 | memcpy reading the engine constants ring back |
| `Constants::Tracker::Capture` | 0.20 | memcpy from the engine's mapping into the ring |
| `Vram::OwnerScope` | 0.30 | `GetModuleHandleW("dxvk_d3d11.dll")` on every scope (native has no DXVK) |
| `LatchBlock::CopyStableRun` | 0.10 | the stages' latch copy (zoned: `CS.Stage.CopyLatch`) |
| `operator new` / heap | 0.2 | spread |

The engine constants ring (`EngineConstants.cpp`) is GPU-visible upload memory, slow to read back. Every captured constant a family read
went through it. Each ring chunk now has a CPU copy: a capture writes the copy and then the chunk from it, and `Bytes` answers the copy.
`Vram`'s DXVK export lookup answers "none" at once in native mode.

| Render thread (native perf-taa) | Before | After |
|---|---|---|
| thread CPU (sampled run) | 6.0 ms | 3.6 ms |
| thread CPU (Tracy, cycles) | 6.66 ms | 4.17 ms |
| in zones | 2.43 ms | 1.96 ms |
| outside zones (CPU minus zones) | 4.2 ms | 2.2 ms (guest 1.4 ms) |
| `Main_RenderWorld` inclusive (sampled) | 2.16 ms | 0.48 ms |

Native is no longer render-thread bound in this scene: the render thread is off the CPU about 2.8 ms a frame (the engine's frame jobs).
The rest of the gap outside zones (about 0.8 ms against guest) is spread: the stages' taps and commits (`TapDeclared`, `SubmitSegment`),
`FrameAnchors::SegmentHolds` and `StepHolds` (string compares, 0.1 ms), the capture's read of the engine's mapping (0.1 ms), heap traffic.

### Before and after (all.sh, perf-taa, measured)

Before: `rn1`, `rn2` (native), `rg1` (guest), at CS `f8fddc4da`. After: `fn1`, `fn2`, `fg1`, at the commit before this doc. One run each, 6 s
Tracy windows; run-to-run spread of the two native runs is given as a range. The machine was in use.

| | Native before | Native after | Guest before | Guest after |
|---|---|---|---|---|
| frame period, mean | 8.06 to 8.25 ms | 6.97 to 6.99 ms | 6.95 ms | 6.95 ms |
| frame period, steady | 7.49 to 7.57 ms | 6.96 to 6.98 ms | 6.95 ms | 6.95 ms |
| render thread busy in zones | 2.33 to 2.35 ms | 1.85 to 1.90 ms | 1.40 ms | 1.37 ms |
| render thread CPU (cycles) | 6.44 to 6.45 ms | 3.96 to 4.02 ms | 2.77 ms | 2.74 ms |
| submission thread busy | 2.38 to 2.41 ms | 2.32 to 2.34 ms | - | - |
| host thread busy | 1.26 ms | 1.71 to 1.73 ms | 0.30 ms | 0.28 ms |
| frames over 20 ms | 6 per window, every 120 frames | 0 | 0 | 0 |
| Begin to last submit, p50 | 6.54 to 6.58 ms | 4.24 to 4.30 ms | - | - |

Both modes now run at 6.95 ms, which is the display's 144 Hz: native is no longer the slower mode here, and its headroom is hidden by
the cap. The host thread's `ORG.Persistent.Retire` grew from 1.13 to 1.60 ms a frame (24 calls): off the critical path, not
investigated. The load and pause-menu transitions (`fload`, `fmenu`) show no new hitch; the menu switch's longest frame is 99 ms (114
before).

### Native cleanup in the same pass

- A stage's one-shot passes rebuilt every anchor of the stage, the per-anchor epochs of its shared anchors among them (unstaged), which
  started Drawcall Limit Fix's short re-entry hold after each load. Now only the anchors those passes sit at are rebuilt. native-quick:
  the first load's hold ends settled (2.6 s) and no re-entry hold follows.
- The phases and regions code is removed (see "State of the native frame").
- Not moved into stages, and left on their current path: the clears before DCLF draws and during its holds, EngineCopy and
  EngineDepthCopy outside the stages' windows, EngineQueries before DCLF draws, the face tint at a load. They run only outside steady
  world frames (a steady world frame is the twelve stage tickets). A menu scene over the live world, blood, precipitation, HDR output and
  frame generation cannot be checked in a background run and stay as they were.

### Left on the render thread (judgement)

- The stages' taps and commits: `CS.Stage.CopyLatch` (0.15 ms, 7 a frame: `LatchBlock::CopyStableRun` copies every block's region and
  sorts a block list each time; copying only the regions a tap wrote needs a written mark per block and run), `CS.Stage.OpenInputs`
  (0.11 ms, zeroes every region), `CS.Stage.MoveSlots` (0.09 ms).
- `CS.Frame.RendererBegin` (0.2 ms self).
- `FrameAnchors::SegmentHolds`/`StepHolds`: client ids compared as strings per tap (0.1 ms); a per-build table of which steps hold which
  client would remove them.
- `ORG.Host.CommitTicket` (about 0.24 ms for 12 commits after this pass) and `CS.DCLF.Texture.ResolveBinding` (0.12 ms, 1,333 calls) were
  not changed.

### The frame cap (2026-11-16)

The 6.95 ms both modes ran at was the display's 144 Hz, from two limiters in the test instance:
- Community Shaders' own: the saved Upscaling `frameRateLimitDivisor` was 1 (refresh / 1 = 144 fps), handed to Reflex as a 6,944 us
  frame limit in both modes. The code's default is already 0 (unlocked); only the instance's saved setting pinned it. It is cleared in
  the instance (`overwrite/SKSE/Plugins/CommunityShaders/SettingsUser.json`).
- SSE Display Tweaks: `EnableVSync=true`, `FramerateLimit=240`, its present hook installed. A test config's `game.uncapped = true`
  (cs-test) writes `SSEDisplayTweaks_custom.ini` (vsync off, tearing on, no limit) into the staged mod for that run only; `perf-taa`
  sets it. The user's ini is not touched.

Not limiters here: CS forces the present's sync interval from its own `vsync` setting (off), so the game's `iVSyncPresentInterval=1`
does not reach either mode; native presents IMMEDIATE (the swap chain allows tearing); `dxvk.conf` sets no frame rate or sync
interval; no `iFPSClamp` or `bLockFramerate`. `CS.Native.PresentSlotWait` is 0.014 ms a frame. The game keeps running unfocused
(`bAlwaysActive=1`), so nothing needs the foreground.

A leftover broke the autoload in between: cs-test's "original" CS mod (`CS AB p35`) had been deleted, every swap back enabled the
missing mod, MO2 then turned `TerrainHelper.esp` off, and the save (which uses it) sat at the loading menu. The swap back now keeps the
variant when the original is gone, and the instance has `CS AB nfg` enabled and the plugin on again.

### Uncapped, native against guest (perf-taa, RTX 5080, measured)

`all.sh` with `PERF_ONLY=1` at CS `7a00afcb9`: `xn1`, `xn2` (native), `xg1` (guest), 6 s Tracy windows, GPU utilization from
`nvidia-smi` in the same window. One guest run; the native spread is the range of the two. The machine was in use.

| | Native | Guest |
|---|---|---|
| frame period, mean | 5.86 to 5.88 ms (170 fps) | 5.10 ms (196 fps) |
| frame period, steady | 5.76 to 5.77 ms | 5.10 ms |
| p50 / p95 / p99 | 5.53-5.72 / 9.11-9.15 / 10.4-10.6 ms | 5.08 / 5.80 / 6.29 ms |
| frames over 20 ms in the window | 2 to 3 | 0 |
| render thread CPU (cycles) | 3.79 to 3.87 ms | 2.72 ms |
| render thread in CS/ORG/DCLF zones | 1.72 to 1.77 ms | 3.40 ms (DCLF's immediate draws) |
| render thread waiting | 1.80 to 1.90 ms (`DrainSubmissions` 1.39-1.56, `WaitTicket` 0.25-0.30) | 0.11 ms |
| submission thread busy / waiting | 2.75 to 2.86 / 2.78 to 2.91 ms (`WaitPresentedFrames`) | - |
| host thread busy (zones) / CPU (sampled) | 2.31 to 2.42 / 0.54 ms | 0.36 ms |
| DCLF scene, workers | 0.65 to 0.66, 1.93 to 1.97 ms | 0.69, 1.92 ms |
| GPU utilization | 69.5 to 71.0% | 63.7% |

Before this pass's cuts (`u1n1`, the cap lifted, one run): native 5.80 ms steady, guest (`u1g1`) 5.11 ms.

What bounds each mode: neither is GPU-bound (64 to 71% busy). Native's chain is render thread -> submission thread -> present: the
submission thread waits about 2.9 ms a frame for presented frames (frame latency 2), and the render thread waits 1.4 to 1.6 ms a frame
in `DrainSubmissions`, called from DCLF's `FrameValues::WaitSignalSubmitted` (the frame values' copy-queue signal waits for graph work
still in the submission thread's queue; the render thread flushes it, which drains that queue). That wait serializes the threads and
leaves the GPU idle part of each frame; it is the likely source of native's 9 to 10 ms periods (p95), not proven. Guest's render
thread is off the CPU 2.4 ms a frame inside the engine's present (DXVK's frame latency wait): it is paced by the GPU and present the same
way, without a mid-frame drain.

### Attribution by thread (native perf-taa, the in-process sampler, measured)

`CS_ORG_RT_SAMPLE=5` now samples the render, submission and host threads (and `dxvk-cs` in guest mode). A sample in a kernel wait is
counted apart; ms a frame below are each class's share of the running samples times the thread's CPU time a frame (cycles). Owner is
the first CS frame from the leaf up by namespace (`rhi::` BasicRHI, `org::` ORG, else CS), unless a driver or API module comes first.
Heap counts come from `AllocCounter`, which replaces CommunityShaders.dll's `operator new`/`delete` (CS, ORG and BasicRHI; the driver's
own heap is not seen); every 16th allocation of a sampled thread keeps its stack, logged as allocation sites. The sampled runs carry the
counter's cost (about 0.1 ms a frame on the submission thread for the stack captures); Tracy runs do not enable it. Run `s6`, the final
build, 848 frames.

Render thread, 3.68 ms CPU a frame (36% of its samples in a wait, nearly all `DrainSubmissions`):

| Owner / kind | ms a frame | Notes |
|---|---|---|
| CS code (hooks, FrameAnchors, stages, tripwire) | 1.59 | `CS.Frame.RendererBegin`, the stage taps and latch copies, the engine constants capture |
| game code | 0.61 | |
| CS DCLF code | 0.40 | the families' latches, landscape seams, texture registry |
| ORG code | 0.29 | `CommitTicket`, `LatchBlock` |
| CS heap | 0.27 | 3,823 allocations, 1.55 MB a frame (before: 5,509, 1.64 MB) |
| hashing, shared_ptr, locks, memcpy, strings | 0.33 | |
| driver/API | 0.06 | |

Submission thread, 2.40 ms CPU a frame (63% of its samples in a wait, nearly all `WaitPresentedFrames`):

| Owner / kind | ms a frame | Share |
|---|---|---|
| driver and API (nvoglv64 1.01, kernel submits 0.20, Streamline and overlay layers 0.05) | 1.26 | 53% |
| ORG (admission and ledger commits, barrier folding, per-pass record dispatch, uploads) | 0.45 | 19% |
| CS and DCLF pass `Record` bodies | 0.34 | 14% |
| BasicRHI wrappers (around `vkCmd*`, submit, present) | 0.18 | 8% |
| heap | 0.10 | 4% (1,310 allocations, 110 KB a frame; before 2,643, 265 KB) |

The submission thread's Tracy zones at the same build (`xn1`): `vkQueueSubmit` 0.37 ms for 16.3 a frame, `BeginPass` 0.24 ms for 225,
`BindPipeline` 0.25 ms for 604, `ResetClaimed` 0.22 to 0.25 ms (driver pool resets). The present path carries OBS's
`graphics-hook64.dll` and the Steam overlay layer (about 0.08 ms each in `vkQueuePresentKHR`).

Host thread, 0.54 ms CPU a frame (94% of its samples in a wait):

| Owner / kind | ms a frame | Notes |
|---|---|---|
| ORG code | 0.20 | retire, ticket completion, preparation |
| CS code (completion callbacks) | 0.13 | |
| heap, strings, std::function, shared_ptr | 0.08 | 476 allocations, 318 KB a frame |
| driver/API | 0.06 | |

`ORG.Persistent.Retire`'s 2.2 ms a frame in Tracy is wall time, not CPU: 23% of the host's samples are blocked in
`SynchronousAdmission::Submitted` (and 5% in `LowestRetainedCompletions`) on the sequencer's mutex, which the submission thread holds
through `PrepareFrame`. It is off the critical path; the host thread does not bound the frame.

### What was cut (largest allocation sites first)

| Where | Thread | Before (allocations a frame) | Change |
|---|---|---|---|
| BasicRHI `cl_barrier` (five vectors per call) | submission | about 330 | per-thread scratch |
| BasicRHI crash markers (a string per marker, `"end: "` per label) | submission | about 380 | look up first; end names interned once |
| BasicRHI `BeginPass` attachments vector | submission | about 240 | on the stack |
| ORG `FoldBarriersOfSkipped`, the late batch's skips and folded batches | submission | about 140 | per-thread buffers kept between recordings |
| ORG barrier range names | submission | about 110 | built in a per-thread buffer, found by view |
| ORG `TypedRenderGraphPass::RevisionHash` | render | about 300 | per-thread vector |
| FrameAnchors `SegmentClients` (a vector of shared_ptr copies per call) and the taps' string compares | render | about 260 | per snapshot, made at Publish; a per-client mask |
| Tripwire `Describe` for the wrapped object (name string, shared_ptr) | render | about 230 | `WrappedOf` and `KindOf` |
| Landscape seams' owner list growth | render | about 600 | reserved once |
| `PublishedResources::ExportedId` to test for an export | render | about 90 | `IsExport` |

| Measured (sampler, one run each) | Before (`s2`/`s3`) | After (`s6`) |
|---|---|---|
| render thread allocations a frame | 5,509 (1.64 MB) | 3,823 (1.55 MB) |
| submission thread allocations a frame | 2,643 (265 KB) | 1,310 (110 KB) |
| host thread allocations a frame | 458 (314 KB) | 476 (318 KB) |
| render thread CPU a frame | 3.92 ms | 3.68 ms |
| submission thread CPU a frame | 2.71 ms | 2.40 ms |
| `CS.Frame.RendererBegin` self (Tracy) | 0.21 ms (`u1n1`) | 0.11 ms (`xn1`, `xn2`) |
| `RecordOwned.BeforeBarriers` (Tracy) | 0.13 ms | 0.10 ms |

The frame period did not move (5.80 ms before, 5.76 to 5.77 after): neither mode is bound by these threads' CPU. Other submission zones
moved up by about 0.1 ms between runs (`ResetClaimed`, a driver pool reset this pass did not touch, by 0.08 to 0.11 ms), which is the
run-to-run spread.

### Left, ranked (estimates)

1. Native's mid-frame drain: `FrameValues::WaitSignalSubmitted` flushes and drains the submission thread's queue so the frame values'
   copy batch can be submitted; the render thread waits 1.4 to 1.6 ms a frame and the GPU idles. Signalling the frame values from the
   submission thread after the graph work it waits for (or letting the copy batch wait on the GPU instead of the CPU) would remove the
   wait. Estimate: the native frame toward guest's 5.1 ms (0.5 to 0.7 ms), and most of native's p95 tail.
2. `SynchronousAdmission::Submitted` takes the sequencer's mutex per queue on the host thread: per-timeline atomics would end the
   contention (the host's 2.2 ms of wall time in `Retire`); no frame-time gain expected.
3. `PublicationBindingBundle` per landscape-seams row and per DCLF texture seal (about 600 allocations a frame on the render thread):
   rows re-put every frame because an unresolved view keeps them changed; a pooled owner list or a retry only on texture-registry
   change. Estimate 0.05 to 0.1 ms render CPU.
4. Native state descriptions (`FindOrDescribe`, about 230 allocations a frame): a native handle is described on every lookup because
   it has no private data; a per-handle cache with release on destroy. Estimate 0.03 to 0.05 ms.
5. BasicRHI `buf_map` maps and unmaps the memory (and a map node) on every `Map` (about 240 a frame on the render thread): persistent
   mapping per memory object, unmapped at free. Estimate 0.05 ms render, plus driver map calls.
6. The submission thread's driver time (1.26 ms) is mostly `vkCmd*` for 225 pass begins and 604 pipeline binds; merging adjacent
   passes with the same targets and skipping a rebind of the bound pipeline would cut calls. Not measured how many are redundant.

### Checks

One background run each, the final build: native-quick, native-d3d12-quick, native-river, native-upscalers, native-interior,
native-validation (the known 20 `VUID-vkCmdDraw-None-09600` only), the loading-thread run, first and third person, guest-quick: passed,
0 tap errors, no `NoPass`, recipe checks 0 findings, 0 frames with a stage absent after it first ran. The frozen-world run
(`Journal Menu=show`) hit the fatal mismatch once in three runs: the capture frame's post segment found the engine's end-of-frame effects
running from target 1 into target 14 where the description had CS's chain (frame 888); the other two passed. These runs are no longer
held at 144 Hz (the saved divisor is cleared), so the capture frame's timing differs from earlier passes; not investigated further.
ORG's unit tests pass run one at a time (the lease tests and HeadlessBenchmark not run); in parallel runs one host test fails at random
(External, then AsyncOpen) and passes alone.

### The present's wait and the threads (2026-11-16, measured)

**What the mid-frame drain waited for.** At the present (DCLF's `FrameDriver::BeforePresent`), `FrameValues::WaitSignalSubmitted` waited
until the frame values' signal was submitted: the copy-queue batch that signals the frame-wait timeline at the frame's value, which every
batch of the frame waits for on the GPU. Before that it drained the submission thread's queue (`FlushPendingStreamWork`). The wait exists
because a present can block until the frame's GPU work is done while it holds the submission lock the upload queue takes (DXVK presents
under it; BasicRHI's Vulkan present holds its timeline lock across `vkQueuePresentKHR`; AMD's windowed present blocks). A signal not yet
submitted then deadlocks. In native mode the drain made the render thread wait until the submission thread had submitted the whole frame
(1.4 to 1.6 ms a frame). The copy batch itself waits on the CPU for its reused copy's point, which is sealed, so already on the queues.

**What replaced it.** With a queued present, the present is an entry on the submission thread after the frame's epochs, so the wait moved
there. `RenderGraphRuntime::WaitBeforePresent` keeps a closure that the queued present runs before the swap chain's present
(`QueuePresent`); a present on the render thread runs it after its drain. Guest mode, held presents and frame generation keep the render
thread's wait. `QueuePresent` now paces before it takes `presentLock`: the present before holds that lock through its frame latency
wait, so the render thread used to wait there outside any zone.

**Native is paced by the GPU.** After present N, the submission thread waits in `WaitPresentedFrames` until frame N-2 is done on the GPU
(frame latency 2), 3.0 to 3.4 ms a frame, with frames N-1 and N already submitted: the GPU always has a frame queued. Guest's render
thread waits the same way inside DXVK's present. `nvidia-smi` reads 62 to 67% in both modes, so it does not measure headroom here.
Removing the drain did not move the steady period. The render thread's wait became the GPU's pacing (`CS.Native.PresentPace`,
`ORG.Host.WaitTicket`), and the tail shrank. The rest of the gap to guest is GPU work per frame (earlier attribution: GENERAL resting
layouts and the ports' cost), not measured further in this pass.

| perf-taa, uncapped, 6 s windows | Native before (`xn1`, `xn2`) | Native after (`f8n1`, `f8n2`) | Guest before / after (`xg1`, `f8g1`) |
|---|---|---|---|
| frame period, steady / mean | 5.76-5.77 / 5.86-5.88 ms | 5.78-5.80 / 5.87-5.90 ms | 5.10 / 5.10-5.11 ms |
| p95 / p99 | 9.11-9.15 / 10.4-10.6 ms | 6.67-6.69 / 9.6-11.7 ms | 5.80-5.89 / 6.29-6.35 ms |
| frames over 20 ms in the window | 2 to 3 | 0 to 1 | 0 |
| render thread CPU (cycles) | 3.79-3.87 ms | 3.64-3.85 ms | 2.70 ms |
| render thread waiting | 1.80-1.90 ms (`DrainSubmissions` 1.39-1.56) | 1.89-2.10 ms (`PresentPace` 1.08-1.52, `WaitTicket` 0.52-0.76) | 0.09 ms |
| `DrainSubmissions` on the render thread | 0.55 a frame | none | - |
| submission thread busy / `WaitPresentedFrames` | 2.75-2.86 / 2.78-2.91 ms | 2.44-2.83 / 3.01-3.38 ms | - |
| GPU utilization (`nvidia-smi`) | 69.5-71.0% | 64.5-67.4% | 63.7% / 62.4% |

The machine was in use (2 to 4% spread between runs). The frames over 20 ms left are GPU stalls: every waiting thread (the copy
uploader's host wait, `WaitPresentedFrames`, the host's completion wait) waits about 20 ms on GPU progress at once, about one frame in
700. No CPU thread is the cause; not attributed further. The load capture (`f8load`) and the pause menu's switch (`f8menu`: longest frame
116 ms against 99 to 114 ms in earlier passes, a GPU completion wait) show no new hitch.

**Other waits (ms a frame, `f8n1`/`f8n2`).**
- Render: `CS.Native.PresentPace` 1.08-1.52 and `ORG.Host.WaitTicket` 0.52-0.76, both the GPU's pacing (a present, or a ticket's
  copy, waits for frames in flight). `CS.DCLF.ListJobsWait` 0.10-0.11, DCLF's list jobs, not changed.
- Submission: `WaitPresentedFrames` 3.0-3.4. Frame latency 2 is the depth DXVK's latency wait gives guest. While the GPU bounds the frame,
  a smaller latency would idle it and a larger one adds a frame of latency for no throughput.
- Copy uploader: `TimelineHostWait` 3.9-4.0, its CPU wait for reused copies' points, off the critical path.
- Host: `CompletionWait` 3.1-3.4, and in its busy time the sequencer's mutex in `SynchronousAdmission::Submitted`. The host thread bounds
  nothing, so the mutex was left as it is.

**Redundant state** (a temporary census in BasicRHI, steady perf-taa, a frame). 625 pipeline binds, 138 of them of the pipeline the list
had bound already (22%). 227 pass begins: 66 with the same attachments as the list's previous pass, 57 of those adjacent with every
attachment loaded and no barrier or dispatch between (candidates for one pass; approximate, copies and clears between were not counted).
Viewports are set about once a frame outside the pass begins (all redundant, negligible). BasicRHI now skips a bind of the bound pipeline
(not in adopted lists; `vkCmdExecuteCommands` and a reset clear it, so no skip crosses a secondary list). `BindPipeline` went from 0.25 to
0.22-0.25 ms a frame and the submission thread's busy time from 2.75-2.86 to 2.44-2.83 ms, within the spread. Merging adjacent passes
needs ORG to see them as one pass; not done.

**The 16.3 `vkQueueSubmit` a frame for 12.3 tickets** (the submission census): 12.3 batch submissions (98 lists; apart from the one
before because of another queue 1.0, the same ticket 3.4, a new wait after an earlier ticket 2.4, waits covered 5.5), 2.0 upload
submissions (a ticket's uploads on the graphics queue, apart from its own batch), 1.0 leading upload (another queue), and the native
present's copy (1.0). The uploads could ride their ticket's first graphics batch as its leading lists; not done.

### Checks (2026-11-16, the present's wait)

Final build, one background run each unless noted: native-quick, native-d3d12-quick, native-river, native-upscalers, native-interior,
first and third person, guest-quick, the guest frozen-world run: passed, 0 tap errors, no `NoPass`, 0 recipe findings, 0 frames with a
stage absent after it first ran. The frozen-world run: 7 in a row passed (6 on the build with the fix, 1 on the final build). The
loading-thread run twice: passed, no copy-queue stall. native-validation three times: two passed with only the known 20
`VUID-vkCmdDraw-None-09600` and back-buffer layout lines. One stopped at its load (`load recent`) on a new mismatch: a menu HDR downsample
draw outside every CS step in a loading-screen frame, while the stages switched in after the load's structural rebuild, just after
Streamline logged barriers whose access masks its stage masks do not support. Not reproduced, not investigated; of this pass's changes
only the bind skip and the pace order run in loading frames. The steady scene: 0 recordings, 0 switches, 0 key builds, no graph build
in the window. ORG's unit tests pass, run one at a time (HeadlessBenchmark not run).

## The load's fatal mismatch: a torn binding capture (2026-11-16, `org-native/phases`)

The validation tour stopped once in three runs at `load recent` on a menu `HDRDownSample4` draw outside every CS step (run
`1116-172731-nfg-native-validation-f5val`). The mismatch was the last link of a chain:
- 30 ms earlier a build of the deferred+water stage (a "one-shot passes ran" rebuild after the load) failed in ORG with
  "Replacement lost a required descriptor snapshot": a slot was bound to a live allocation with no descriptor views.
- The frame anchors' failure handler dropped every client of that build (the menu composites among them) "back to D3D11". Native mode
  has no D3D11 path, so the next menu scene's end-of-frame draw reached the frame with no pass: the fatal tap error. The frame kind
  (loading screen) and its recipe (menu UI) were right; the stage's schedule was not lost to the rebuild.

The cause is a race between graphs of the shared alias arena. The failing build's bootstrap ran on the build thread (it gathered 85
passes at 27.172 and never logged its bootstrap) while the host thread placed the previous deferred+water build (`alias plan epoch=41
... rematerialized=8`, 27.174 to 27.176). A re-placement dematerializes a transient (its descriptor slots rotate away) and materializes
it again. `PublicationBindingBundle::Capture` took the allocation under the resource's materialization lock, then read its views
without it, so a capture between the two halves paired the old allocation with no views (this failure), or with the next backing's
views (a silent wrong binding). `BufferBase` did not lock its dematerialize or its capture at all.

The fix (ORG):
- `BackedResource::BackingMutex`: the materialization lock (now recursive) of `PixelBuffer` and `BufferBase`. A publication capture
  holds it across the allocation, the views and the descriptor owner; `BufferBase` locks its dematerialize and capture; the rotation
  keys read a backing's generation and views under it.
- A regression test (`RunCaptureDuringReplacementScenario` in RebindingGpuTests, Vulkan host tests `rebinding`): one thread
  dematerializes and materializes a texture 400 times while another captures it; no capture may have an allocation without views.

And CS: in native mode a failed stage build stops the game with the build's own error (development builds, the mismatch rule's switch
`CS_ORG_MISMATCH_FATAL`), instead of dropping its clients to a D3D11 path native mode does not have and failing later on a mismatch
that hides the cause.

Checks: 8 `load recent` loads clean (4 without validation, 4 with), plus their startup loads; 126 one-shot rebuilds and 52
rematerializing alias plans ran during them.

## GPU cost (2026-11-16, `org-native/phases`)

### GPU timing in native

The graph's GPU timestamps were empty in native because every native stage is a recording kept across frames, and kept recordings are
timed only with `CS_ORG_RECORDING_TIMESTAMPS=1`, which nothing set: `CS_GPU_EVENT_TIMERS` and the idle trace got frames with no
ranges. Per-pass timing in kept recordings also ends every range with a drain (NVIDIA's bottom-of-pipe timestamp otherwise lands before
a preceding ExecuteIndirect), which distorts a frame of 600 ranges.

Now (ORG `RenderGraph::RecordingTimestampMode`): `Lists` times each kept list once, from before its entry barriers to after its exit
barrier, one drain per list; `Passes` is the old per-pass and per-barrier mode. CS turns `Lists` on whenever the graph's timings are
collected (`CS_GPU_EVENT_TIMERS` on the graph, which native always is, or native's idle trace); `CS_ORG_RECORDING_TIMESTAMPS=1` asks
for `Passes`, `=0` for none. Off otherwise: kept recordings carry no queries. The timestamps are part of the kept recording; each
submission resolves them and its frame slot's completion reads them. Each range is charged to the stage whose epoch submitted it
(`GpuTimingSource`), and the `[GpuEvents]` log has a row per stage before the rows per range. The cost of list mode is inside the
run-to-run spread (5.91 ms with it, 5.77 to 5.98 ms without).

`CS_RHI_CENSUS=<frames>` logs BasicRHI's command census (`rhi::census`), per frame by list (labelled with its first pass): render
passes, attachments and their load and store operations, adjacent passes on the same attachments, barrier commands, image barriers
(layout changes, same state, onto the previous pass's attachments), buffer and memory barriers.

### Per stage (perf-taa, RTX 5080, uncapped, background runs)

Native: list timestamps, mean of 5 blocks of 300 frames, one run (`gt1`; `gt2` within 0.03 ms on every row). Guest: its D3D11 event
timers (`ggt1`) for the engine's work, and its graph ranges (`ggt2`, `CS_GPU_TIMERS=graph`) for DCLF's epochs, one run each. Guest runs
terrain shadows and Cloud Shadows' layer copies on D3D11, so its D3D11 timers cover them. Measured unless marked.

| Stage | Native GPU ms | Guest GPU ms |
|---|---|---|
| start (reflection faces' sky, water effects, Cloud Shadows' layer copies) | 0.37 | 0.20 (Water Effects, CloudShadows' reflections prepass) |
| DCLF shadows | 0.55 | 0.48 |
| pre-depth (terrain shadows, cloud layers, skylighting probes) | 0.26 | about 0.02, and 0.07 skylighting under World (estimated mapping) |
| DCLF depth (Z-prepass) | 0.82 + 0.03 async | 0.74 + 0.02 async |
| pre-main (depth copy, shadow mask, LLF, SSS raymarch, sky) | 0.39 | about 0.33 (estimated from its events) |
| DCLF opaque | 0.90 | 0.94 |
| deferred+water | 1.02 | about 0.9 (World less DCLF's and the prepasses; estimated) |
| DCLF forward | 0.08 | 0.09 |
| sun | 0.004 | - |
| DCLF first person | 0.02 | 0.02 |
| post (TAA, upscale copy, tonemap, VL composite, chain) | 0.80 | about 0.8 (post events 0.3, TAA and present 0.5; estimated) |
| UI (Scaleform) | 0.01 | - |
| engine anchors (shadow clears, frame clear, queries) | 0.02 | - |
| sum of stages, graphics queue | 5.23 | |
| graphics queue busy / first to last range | 5.28 / 5.94 | GPU frame 5.08 |

Frame period on the same build (Tracy, steady): native 5.77 and 5.98 ms (`b1n1`, `b1n2`), guest 5.26 ms (`b1g1`). Both are GPU-bound:
guest's GPU frame (5.08) is its period, and native's graphics span (5.94) is its period.

Largest native passes (per-pass mode, inflated by its drains): DCLF colour 0.76, DCLF shadow view 0.43, DCLF Z depth 0.40, VL generate
0.22, SSGI 0.22, Cloud Shadows' sky-reflection gather 0.17 and scatter 0.11, terrain shadows' update 0.11, skylighting probes 0.09,
DCLF reflection faces 0.09. Every barrier batch range reads 0.00.

### What makes up the gap (about 0.6 ms a frame on this build)

| Cause | Cost | How |
|---|---|---|
| Cloud Shadows' native path (layer copies as compute: 6 face taps x 32 layers, gather and scatter) | 0.2 to 0.35 ms | measured by removal: native 5.55 and 5.61 ms without Cloud Shadows (4 runs with it: 5.77 to 5.98), guest 5.27 against 5.26 |
| GPU idle between lists on the graphics queue | 0.66 ms span less busy, of which about 0.2 is the timestamps' own drains (estimated) | measured, idle trace (`idle1`): 47 lists, two cross-queue waits (0.13 ms before DCLF's Z depth, 0.08 ms before the cubemap publish) and 44 list boundaries of median 6 us |
| DCLF Z-prepass and shadow view slower than in guest | +0.08 and +0.07 ms | measured, the same graph timers in both modes |
| terrain shadows' update | 0.12 ms against guest's 0.01 | measured, the same dispatch on both paths; cause not found |

These overlap (the idle and the removal both include Cloud Shadows' list boundaries), so they are not added up. Not causes (measured):
- DCLF's async compute: with `CS_DCLF_ASYNC_COMPUTE=0` native runs 5.92 ms (inside the spread); the 0.13 ms wait moves, as found
  before.
- GPU gates: with `CS_ORG_GPU_GATES=0` (gated passes recorded at execution, closed ones left out) native runs 5.89 ms.
- Recording only the open entries of Cloud Shadows' layer copies and terrain shadows' stripes (at execution; tried and reverted):
  start and pre-depth did not move (0.38 and 0.28 ms). Their cost is the work of the entries that run, not the empty dispatches.

### The pass count (the user's hypothesis)

Census, steady perf-taa frame (measured, `cen1`, `gcen1`):

| A frame | Native | Guest (DXVK's `DXVK_BARRIER_STATS`) |
|---|---|---|
| render passes | 274 (129 of them the reflection faces' sky, 21 water, 17 shadow cascades, 17 SSR) | 75 |
| ... on the same attachments as the list's pass before, no barrier between | 75 | - |
| colour / depth attachments | 457 / 103, all loaded and stored but 18 clears | - |
| attachments in GENERAL | 0 | - |
| barrier commands | 297 | 166 |
| image barriers (layout changes) | 663 (589) | 74 (64) |
| same-state image barriers (unordered access after unordered access, entry write after write) | 72 | - |
| buffer barriers | 653 | (in the 166 commands) |

The counts are much higher in native, but the GPU time says they do not carry the gap in this scene: the 129 sky passes take 0.054 ms
together (list timestamp), the UI stage (Scaleform's instances, one display drawn) 0.01 ms, the post chain matches guest's, and no
barrier batch range reaches 0.01 ms. Merging the 75 mergeable passes would save about 0.03 ms at most (75 x about 0.4 us, estimated).
Keeping a render pass open across such a boundary was not tried: BasicRHI drops commands that need the pass closed (a dispatch inside
an open pass returns), so a deferred end needs every such command audited first. Batching Scaleform and the post chain was not done
for the same reason: the passes it would merge cost too little to move the frame. The menu UI stage (340 to 380 passes) was not timed
in a steady menu.

Same-state attachment barriers: ORG already records no barrier between two passes of one list that use an image in the same layout,
access and sync (render target after render target, depth write after depth write: `NeedsTextureBarrier`). Same-state barriers are
kept only for unordered access after unordered access, and at an execution's entry for a write after a write
(`NeedsEntryTextureBarrier`). Nothing was elided further: across render passes Vulkan requires a dependency for an attachment write
after a write (submission order does not order them), so the existing in-list elision is itself worth a run with synchronization
validation.

### Left, ranked (estimates)

1. Cloud Shadows' layer copies in native (0.2 to 0.35 ms): a few copies of a 256x256 face per layer each face tap, done as compute
   dispatches over all six faces through a scratch array. Plain texture copies of only the faces that change (recorded at execution
   from the commit's rows) would bring it toward guest's cost.
2. GPU idle at list and submission boundaries (0.2 to 0.45 ms): 16 submissions a frame; the uploads riding their ticket's first
   graphics batch would remove 3; the two cross-queue waits (0.21 ms) need DCLF's async work to start earlier, not to be turned off.
3. Terrain shadows' update (0.1 ms): the same stripe costs 12 times guest's D3D11 dispatch; the R16 heightmap the ORG path makes is
   the first suspect.
4. DCLF Z-prepass and shadow view in native (0.15 ms together).
5. Image layout changes (589 a frame against 64): not measured as a cost on NVIDIA; likely to matter on AMD.

### Checks (2026-11-16, GPU cost)

Final build (DLL f8c2f5c2d391), one background run each unless noted: native-quick, native-d3d12-quick, native-river,
native-upscalers, native-interior, the loading-thread run, first and third person, the frozen-world run, guest-quick: passed, 0 tap
errors, no `NoPass`, recipe checks 0 findings, 0 frames with a stage absent after it first ran, no failed build. native-validation
three times: passed, only the known 20 `VUID-vkCmdDraw-None-09600` and back-buffer layout lines. Steady scene: 0 recordings, 0
switches, 0 key builds. The quick run's shot matches the earlier one (same brightness). ORG's unit tests pass run one at a time, and the
Vulkan host tests' `rebinding` (with the new capture test) and `split` modes; `pipelines` did not finish in 240 s (HeadlessBenchmark
not run).

### Second pass (2026-11-16, Cloud Shadows, terrain shadows, async compute)

Measured, perf-taa, RTX 5080, uncapped, background runs, the machine in use. "Before" is the build of the section above
(DLL f8c2f5c2d391) re-taken in this session (`a0*`), "after" the final build (DLL 434e083dc9d4, `f1*`).

**Cloud Shadows' layer operations.** What native did that guest does not: the queue of clears and copies became a gathered and a
scatter dispatch per layer and per face tap, each over all six faces (the rows marked the faces with no work, which still got
their groups): 6 taps x 32 layers x 2 = 384 indirect dispatches in the start stage, plus 64 at each of the two other anchors.
The gather pass took 0.17 ms with one or two layers open per tap (per-pass mode); the scatter 0.12 ms. A frame here queues about
34 face writes (28 of them clears, about 13 cloud layers a face in this weather) and 3 face reads, on 2 face taps.
- Now each tap's commit writes one list of entries (layer, face, scratch slot or clear), and the gather and the scatter are one
  dispatch each per tap, a group's z picking an entry (`LayerOpsCS.hlsl`). Every layer is bound by its whole-array UAV, so one
  dispatch reaches any layer; the 32 handles are packed into 24 push words (the block's limit is 32 words). Commit `927baf26d`.
- Gather 0.17 to 0.01 ms, scatter 0.12 to 0.05-0.10 ms (it varies with the weather's layers). Start stage 0.36 to 0.28 ms.
- Tried and reverted: a 4x4 block of texels per thread (no change); clearing a layer face as the load operation of the sky
  family's first draw into it, in place of the scatter's clear (the cloud passes went from 0.03 to 0.11 ms, start to 0.37 ms).
- Cloud Shadows' cost by removal (`features.CloudShadows=false`, GPU time): native 0.30 ms (graphics sum 5.17 to 4.87; start
  0.28 to 0.06), guest 0.16 ms (GPU frame 5.03 to 4.87; its cubemaps 0.19 to 0.04). Before: native 0.2 to 0.35 ms by frame
  period. What is left in native is the scatter's clears (UAV writes over whole faces; guest's D3D11 clears are DXVK's) and the
  layers' barriers: about 0.14 ms over guest.

**Terrain shadows' update.** Native recorded every stripe a refresh may need (up to 32), each behind a full memory barrier, in the
kept recording; a steady frame opens one stripe, so it ran 31 full barriers around empty indirect dispatches. The R16 heightmap
was not the cause. Now the update pass records stripe 0 ahead, and a refresh pass with the other stripes is recorded at execution
and skips a run that did not open stripe 1 (its late batch is kept while the answer holds: 8.0 inline lists kept a frame against
7.0, 0 recordings in the steady scene). Commit `f7da0d38c`. Update 0.11 to 0.02 ms (guest 0.01), pre-depth 0.27 to 0.16 ms.

**The two cross-queue waits.** The 0.13 ms wait before `cs.dclf.z.depth` was DCLF's compute-queue passes (tree wind, the fade and
portal batches: 0.03 ms of work): the graphics queue waited for the compute batch, which ran only after its own cross-queue
waits. With them on the graphics queue the gap is gone from the idle trace and the period drops (alternating runs: 5.51 and 5.56
ms against 5.66 and 5.63 ms). Native mode now defaults `CS_DCLF_ASYNC_COMPUTE` to the graphics queue (`1` keeps the compute
queue; guest unchanged). Commit `678056f3d`. The 0.08 ms wait before `cs.dyncubemaps.publish` remains: it is not DCLF's compute
queue (it stays with async compute off); cause not found.

**Submissions.** `vkQueueSubmit` a frame: 12.3 batches + 2.0 uploads + 1.0 leading upload + 1 present copy (16.3) before, 10.2 +
2.0 + 0 + 1 (13.2) after: the leading upload and the compute batches went with the compute queue. Letting a ticket's 2.0 graphics
uploads ride its first graphics batch was not done: batches on other queues wait on the uploads' own signal, which a merged
submission would move to the end of the first group.

**Idle.** Span less busy (idle trace, timing on): 5.91 - 5.32 = 0.59 ms before, 5.58 to 5.67 - 5.10 to 5.19 = 0.48 ms after, one
gap over 0.05 ms left (the 0.08 ms above), the rest list boundaries. With timing off the graphics span is the frame period, 5.47
to 5.54 ms, against a busy time of about 5.1 ms: about 0.3 to 0.4 ms of idle (estimated).

**Per stage, GPU ms (list timestamps, same session):**

| Stage | Native before (`a0gt`) | Native after (`f1gt`) | Guest (`f1ggt`, `f1ggd`) |
|---|---|---|---|
| start | 0.36 | 0.28 | 0.19 (Water Effects' cubemaps, Cloud Shadows in them) |
| DCLF shadows | 0.59 | 0.60 | 0.48 |
| pre-depth | 0.27 | 0.16 | about 0.02, and 0.07 skylighting (estimated mapping) |
| DCLF Z-prepass (graphics + async) | 0.86 + 0.03 | 0.89 + 0 | 0.73 + 0.02 |
| pre-main | 0.39 | 0.39 | about 0.33 (estimated) |
| DCLF opaque | 0.94 | 0.93 | 0.94 |
| deferred+water | 1.04 | 1.04 | about 0.9 (estimated) |
| DCLF forward | 0.08 | 0.08 | 0.09 |
| post | 0.78 | 0.78 | about 0.8 (estimated) |
| graphics sum / span | 5.34 / 6.02 | 5.17 / 5.73 | GPU frame 5.03 |

Frame period (Tracy, steady): native 5.84 and 5.80 ms, guest 5.11 ms before; native 5.54 and 5.47 ms, guest 5.07 ms after. The
gap went from 0.69-0.73 to 0.40-0.47 ms.

**DCLF Z-prepass and shadow view (not changed).** Same draws in both modes, but not the same ranges: native's shadow view list
holds the engine's shadow-map clears as render passes of their own (`cs.engine.skeleton.shadow.depth2.0/1@shadow-view`, 0.02 ms
each, about 0.05 ms in all), which guest does as D3D11 clears outside DCLF's timer (DXVK folds a whole-image clear into the next
render pass's load operation). That is most of the shadow view's +0.07 to +0.12 ms. The Z-prepass now also holds the 0.03 ms of
compute passes. Folding the engine clears into DCLF's first pass's load operation was not done.

**start, pre-depth, deferred+water by pass (per-pass mode, not changed).** Start's rest is the sky faces and their cloud draws
(0.01 to 0.04 ms a pass) and the water instances (under 0.01 ms each). deferred+water's largest passes have guest counterparts
doing the same work (SSGI 0.22, VL generate 0.22, skylighting probes 0.09, deferred composite 0.07); no pass without a
counterpart over 0.01 ms was found.

**Checks left open.**
- Synchronization validation: the validation tour with `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true` (layer 1.4.357) passed with no
  `SYNC-HAZARD` message and only the known 20 `VUID-vkCmdDraw-None-09600` and back-buffer layout lines. ORG omits a barrier
  between two passes of one list that use an attachment in the same layout and access (render target after render target, depth
  write after depth write); it relies on queue order and rasterization-order attachment writes. Syncval did not flag those
  boundaries. It cannot see shader accesses through the descriptor heap, so this covers attachments and transfers only.
- ORG's Vulkan host test `pipelines`: it hung once in this session (after "input layout build", in "permutations"), during a full
  CS build, as the earlier 240 s timeout did; 46 later runs passed in 1 to 4 s, under CPU, disk and GPU load. Not reproduced, so
  no dump and no fix; not caused by this pass (no ORG change).

**Left, ranked (estimates).**
1. Idle between lists and the one cross-queue wait (0.3 to 0.4 ms with timing off): 46 list boundaries, the 0.08 ms wait before
   `cs.dyncubemaps.publish`.
2. Cloud Shadows' scatter clears and the layers' barriers (about 0.14 ms over guest).
3. The engine's shadow-map clears as separate passes in DCLF's shadow view list (about 0.05 ms), and start's sky faces.
4. pre-main and deferred+water against guest (about 0.05 and 0.1 ms, with estimated guest mappings).
5. The menu UI stage was not timed in a steady menu.

### Third pass (2026-11-16, where the GPU goes idle)

Measured, perf-taa, RTX 5080, uncapped, background runs, the machine in use. "Before" is CS `73763c806` with the trace tooling
added (no change in behaviour), "after" the final build (DLL 7d3dbc609994). Native and guest were taken in the same session.

**The tool.** `CS_SUBMIT_TRACE=<file>` with `CS_GPU_IDLE_TRACE=<frames>` (native) turns on BasicRHI's submission trace
(`rhi::submittrace`). At each analysed frame it appends six frames of list ranges and every queue submission of the last 400 ms:
the CPU time around the driver call (QueryPerformanceCounter), the queue, the command lists by handle, the timeline waits and
signals, and the present relay, present and acquire. A kept list's range name ends with its handle, so each range is matched to
the submission that ran it. Calibrated timestamps (now enabled whenever the device has them) map GPU ticks onto the CPU clock.
With `CS_RHI_CENSUS` on as well, each list carries its census label and counts, so the lists between two timed ranges are named.
The analysis is a script (`subtrace.py`, in the session's scratchpad): per gap, the lists and submissions between the two ranges,
the waits of the next submission, when the signal of each wait was submitted, and when the CPU submitted the next list.

**What the gaps are.** Steady perf-taa, 18 frames, timing on. 46 gaps a frame between timed ranges.
- (c), the GPU starved by the CPU: none. Every list was submitted 11 to 20 ms before the GPU reached it. With frame latency 2 and a
  GPU-bound frame the submission thread is always more than a frame ahead.
- (b), waits on other queues: none that blocks. The batches wait on the copy queue's frame values ("CS frame values") and on the
  external work slots, all signalled by submissions made 11 to 19 ms earlier.
- The 0.08 ms "wait before `cs.dyncubemaps.publish`" was not a wait. The lists between `cs.dyncubemaps.inferred-mips` and the publish
  were two handoff lists and the BC6H copies, a kept late list with no timing range: 96 copy regions and 18 layout changes a frame,
  69 us (timed with `CS_ORG_KEEP_GATED=0`). The same held for the filtered mip 0 copy before `inferred-mips` (17 us).
- Kept gated late lists are untimed. Their work shows as idle: the volumetric lighting depth copy (23 us), the G-buffer clears
  (8 us), the grass colliders' copy (5 us), the terrain depth clear, subsurface clear and terrain shadows' refresh (2 to 3 us each).
- What is left is the cost of a boundary. Measured in place:
  - an extra empty command buffer per list (`BASICRHI_PAD_LISTS=1`, timing on) adds 0.4 us a boundary; eight per list
    (`=8`, timing off) add 0.17 ms a frame for about 800 lists, 0.2 us each;
  - every list in a submission of its own (`BASICRHI_SPLIT_SUBMITS=1`) adds 2 to 3 us a boundary with timing on, and 0.11 ms a
    frame for about 85 extra submissions with timing off, 1.3 us each;
  - binding the descriptor heaps five times per list (`BASICRHI_HEAP_BIND_REPEAT=4`) changes nothing (5.54 against 5.48-5.54 ms);
  - the timestamps themselves: 5.59 ms with list timing against 5.48 to 5.54 ms without, 0.05 to 0.1 ms.
  So of the 3.6 to 5 us between two adjacent timed lists, the command buffer is 0.2 to 0.4 us and the timestamps about 1 to 2 us.
  The rest is the pipeline draining at the boundary and filling again (each list ends after a drain when timed, and a handoff's
  barrier drains too).

The gap table (mean us a frame, timing on; before `t2`, after `t8off`; "submits" counts the queue submission boundaries crossed).
Cause: (a) boundary, or the work named.

| Previous list -> next list | Before | After | Between them | Cause |
|---|---|---|---|---|
| dyncubemaps.inferred-mips -> dyncubemaps.publish | 80.2 | 9.2 | handoff, BC6H copies, handoff | untimed copies (gated now; in copy frames the copies are timed: 4.3 + 4.4 us gaps) |
| grass-collision.update -> skeleton.frame.depth0.0@z-prepass | 33.4 | 32.1 | 2 submits: upload (27 copies), handoff (64 buffer, 16 image barriers) | the ticket's uploads and entry barriers (1 submit after) |
| shared.upload.1 -> shared.upload.2 | 30.9 | 31.3 | volumetric lighting depth copy | untimed copy, 23 us |
| subsurface.extract -> dyncubemaps.inferred-mips | 27.3 | 9.5 | handoff, filtered mip 0 copy, handoff | untimed copy (gated now) |
| scaleform.draw.0.i0 -> cloud-shadows reflections-prepass gather | 20.7 | 21.5 | 5 submits: Scaleform's kept lists, present relay, present, acquire, handoff | the frame boundary |
| imagespace.start.water.texoffset.8 -> skeleton.shadow.depth2.0@shadow-view | 19.6 | 17.7 | 2 submits: upload, handoff (105 buffer, 14 image barriers) | uploads and entry barriers (1 submit after) |
| skeleton.shadow.depth2.0@shadow-view -> cloud-shadows early-prepass gather | 19.0 | 17.0 | 1 submit: handoff, terrain depth clear, handoff (37 layout changes) | entry barriers, untimed clear |
| screen-space-shadows.raymarch -> shared.upload.5 | 18.3 | 16.2 | handoff, G-buffer clears | untimed clears |
| volumetric-shadows.blur-v-mip0 -> grass-collision.update | 15.3 | 12.9 | handoff, colliders' copy, handoff | untimed copy |
| shared.upload.0 -> subsurface.extract | 12.0 | 11.2 | subsurface clear, handoff | untimed clear |
| cloud-shadows early-prepass gather -> volumetric-shadows.blur-v-mip0 | 10.8 | 10.3 | terrain shadows' refresh, handoff | untimed late list |
| dclf.latched-copies -> shared.upload.0 | 10.7 | 10.1 | 1 submit: handoff | (a) submit and entry barriers |
| skeleton.frame.depth0.0@z-prepass -> skeleton.depth-copy | 10.3 | 10.3 | 1 submit: handoff | (a) |
| 7 more ticket boundaries (forward, first person, sun, capture -> Scaleform) | 7.4 to 8.0 each | 7.0 to 8.4 | 1 submit: handoff | (a) submit, handoff, drain |
| 22 boundaries inside a submission with a handoff between | 4.8 to 6.7 each | 3.6 to 5.9 | handoff | (a) list, handoff, drain |
| 9 boundaries inside a submission, adjacent lists | 3.6 to 5.0 each | 3.7 to 4.9 | nothing | (a) list and drain, timestamps |
| sum | 0.49 ms | 0.37 ms | | span less busy |

**Changes.**
- Dynamic Cubemaps (CS `95a96582e`): the filtered mip 0 and the two BC6H copies ran at every run though their sources change only
  at an inference or a publish. A commit now opens a copy's gate entry only while its source changed since the copy last recorded,
  and keeps it open while the copy is not yet in the running graph. About 0.09 ms of GPU work a frame, in guest too (the same pass).
- Merged handoffs (ORG `937a2e1`, CS `0fc36c324`, `CS_ORG_MERGE_HANDOFFS`): with coalesced submissions the entry barriers of a queue
  submission's batches go into one handoff list ahead of its first batch. A batch's entry barriers touch only resources the earlier
  batches of the ticket do not use, and every wait of the submission comes before its lists. Barriers that discard, start from
  Undefined or transfer a queue family stay with their batch. Handoff lists 40.5 to 11.2 a frame.
- Uploads with batches (same commits, `CS_ORG_UPLOADS_WITH_BATCHES`): a ticket's upload lists go in its first queue submission
  when all its batches are on the graphics queue (in native, all of them since the compute work moved there), instead of a
  submission of their own. Upload submissions 2.0 to 0 a frame.
- Together: graphics `vkQueueSubmit` 13.2 to 9.2 a frame (plus the present relay), command lists 98.7 to 69.7. The merge pair
  alone (A/B in one session, `t7off` and `t7b`): span less busy 0.42 to 0.38 ms with timing on, the period inside the spread.
- Not done: merging consecutive tickets' submissions further. The census still counts 4.8 submissions a frame that could have
  joined the one before (no new wait) had they been queued together; they are submitted as they are committed, and the submission
  thread never holds one back. 1.3 us each.
- Not done: one command list per ticket (secondaries for the kept batches, the late passes and handoffs recorded into one primary).
  A command buffer boundary measured 0.2 us; the 70 left cost about 14 us a frame.

**Clears as load operations (part 3): not sound on this GPU.** The shadow map slices' clears (a gated fullscreen draw each) were
turned into a render pass whose load operation clears (`CS_ORG_LOAD_OP_CLEARS`, tried and reverted). The clear itself got cheaper,
but every later reader of the shadow maps got slower: DCLF's opaque list +0.33 ms, the water surface +0.17, DCLF forward +0.10,
the sky +0.09; the frame +0.6 ms (A/B `t8off`, `t8on`). A fast-cleared depth image is decompressed when it moves to a read layout.
The cloud layers' load-operation clears of the second pass got slower the same way (0.03 to 0.11 ms). The menu scene's case
(first writers that can be empty) was not revisited. Left: every engine clear stays a gated draw; DXVK keeps its clears in its own
way and was not changed.

**Cloud Shadows (part 4): not changed.** The 28 face clears a frame come from `NoteGraphCloudLayer`: each layer the graph draws on a
face is cleared first, because the graph draws the face's earlier layers into it again, where the D3D11 path copies the layer below.
They are needed every frame the layer is drawn, the cloud draws blend, so the scatter cannot skip texels, and a load-operation clear
is the slow path above. Copying the layer below (the D3D11 algorithm) needs the layers' draws reordered. About 0.14 ms over guest
remains.

**Before and after, same session (timing off, Tracy, steady frames).** Native before 5.48 to 5.54 ms (`n0`, `n0b`, `n1`), guest
5.07 ms (`g0`). Native after 5.40 to 5.58 ms (`an1` to `an4`), guest 5.10 to 5.19 ms (`ag1`, `ag2`), taken an hour later with the
machine busier (guest's own period moved by 0.12 ms). The merge pair off in the same window (`anoff`): 5.62 ms. The period did not
move outside the spread: the changes remove about 0.1 ms of GPU time with timing on (the cubemap copies 0.09, the boundaries
0.04), which the 2 to 4% run-to-run spread hides. The gap to guest is 0.21 to 0.48 ms across the windows.

| | Native before | Native after | Guest |
|---|---|---|---|
| frame period, steady | 5.48-5.54 ms | 5.40-5.58 ms | 5.07 ms before, 5.10-5.19 ms after |
| graphics `vkQueueSubmit` a frame (and the present relay) | 13.2 + 1 | 9.2 + 1 | - |
| command lists a frame | 98.7 | 69.7 | - |
| span less busy, timing on | 0.49 ms | 0.37 ms | - |
| idle with timing off (estimated) | about 0.3 ms | about 0.2 ms | - |

Per stage, GPU ms (list ranges from the traces, timing on; the cubemap copies were untimed before, so the stages do not show them):

| Stage | Before (`t2`) | After (`t8off`) |
|---|---|---|
| deferred+water | 1.04 | 1.05 |
| DCLF opaque | 0.99 | 0.94 |
| DCLF Z-prepass | 0.89 | 0.88 |
| post | 0.78 | 0.78 |
| DCLF shadows | 0.59 | 0.57 |
| pre-main | 0.39 | 0.39 |
| start | 0.30 | 0.29 |
| pre-depth | 0.16 | 0.16 |
| DCLF forward | 0.08 | 0.08 |
| sum / span | 5.23 / 5.73 | 5.17 / 5.54 |

**Left, ranked (estimates).**
1. Drains at list boundaries (about 46 a frame, 1 to 3 us each with timing off): fewer, longer lists would need ORG to put a ticket's
   batches into one command list and to merge a list's leading entry barriers with the previous list's tail.
2. Untimed kept late lists' work (the volumetric lighting depth copy 23 us, clears 15 us): real work, partly avoidable (the G-buffer
   and subsurface clears could be skipped where the first writer covers the target, not checked).
3. Cloud Shadows' clears and layer barriers (0.14 ms over guest).
4. Submission and command buffer overhead now about 0.03 ms a frame (9 submissions at 1.3 us, 70 lists at 0.2 us).
5. The frame boundary (present relay, present, acquire: 21 us) and the uploads' entry barriers (17 to 32 us at two tickets).

**Checks.** Final build, one background run each: native-quick, native-d3d12-quick, native-river, native-upscalers,
native-interior, the loading-thread run twice, first and third person, the frozen-world run, guest-quick: passed, 0 tap errors,
no `NoPass`, recipe checks 0 findings, 0 frames with a stage absent after it first ran, no failed build. native-validation: passed, only the known 20 `VUID-vkCmdDraw-None-09600` and back-buffer layout lines;
again with `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`: passed, no `SYNC-HAZARD`, the same lines. Steady scene: 0 recordings,
0 switches, no graph build in the window. The MO2 instance keeps `+CS AB nfg` and `*TerrainHelper.esp`, and every run loaded into
the world on its own. The quick
run's shot matches the earlier one (105.6 against 105.3 mean). ORG's unit tests pass, run one at a time, and the Vulkan host test's
modes sync, async, reuse, revision, segment, external, external-reuse, split, built, stream, arena, rebinding, contributor, inline
and pipelines (the lease modes and HeadlessBenchmark not run).

### RX 6600 (2026-11-17, `org-native/phases`)

Measured on the RX 6600 (driver 32.0.21045.16010, Vulkan 1.4.315), perf-taa at 1920x1080, uncapped, background runs with
`DXVK_DEBUG=hang`, VRAM Budget off in both modes (`features.VRAMBudget=false`: it unloaded cells in native, which used more memory).
The RTX 5080 is no longer in the machine; the AMD iGPU is, with no display on it. Native's BasicRHI device prefers the discrete GPU
and guest's DXVK picks it too; every run's log names `AMD Radeon RX 6600`. The one display (2560x1440, 60 Hz) is on the RX 6600; the
game runs borderless at 1080p and presents IMMEDIATE in both modes. Frame periods are not quantized to 16.67 ms, so no vsync applies.
GPU busy comes from the Windows `GPU Engine` counters for Skyrim's process (nvidia-smi does not apply).

**Missing geometry first (all earlier RX 6600 numbers of this session are void).** The first runs drew almost nothing in the mid
field: buildings, fences and trees beyond the nearest one were gone, in native and guest alike, from the first frame. With DCLF off,
or with `CS_DCLF_CULL=frustum`, the scene was whole. The test instance's saved settings held `CullMode: 2` (occlusion): DCLF wrote
the cull mode back on every save, so the RTX 5080 sessions stored that GPU's default. On the RX 6600 the saved value overrode the
AMD default (frustum only), and the HZB reads about 0 on this driver (still open, `dclf-open-defects.md`), so occlusion culled
nearly everything in view. Now the cull mode is saved only when it is not the machine's default, and a saved occlusion mode is not
applied where the default leaves it out (a warning says so; `CS_DCLF_CULL=occlusion` still forces it). CS `733de3045`. The RTX 5080
numbers above are not affected: occlusion works there, and its shots show the mid field.

**Baseline (Tracy, 6 s windows, alternating runs; native twice, guest twice).**

| | Native, before the fixes (`b2n*`) | Native, after (`b3n*`) | Guest (`b3g*`) |
|---|---|---|---|
| frame period, mean | 20.99 / 21.09 ms | 19.66 / 19.49 ms | 19.33 / 19.30 ms |
| steady mean | 20.99 / 20.99 ms | 19.53 / 19.49 ms | 19.33 / 19.30 ms |
| p50 / p95 / p99 | 20.86-21.01 / 22.8-22.9 / 24.9 ms | 19.44-19.55 / 20.56-20.58 / 21.4-21.7 ms | 19.29-19.31 / 22.04-22.10 / 23.8-24.2 ms |
| frames over twice the median | 0 / 1 | 1 (57.8 ms) / 0 | 0 / 0 |
| render thread CPU (cycles) | 4.56 / 4.66 ms | 4.51 / 4.47 ms | 3.82 / 3.88 ms |
| submission thread busy | 2.17 / 2.24 ms | 2.23 / 2.22 ms | - |
| GPU busy (Skyrim's 3D engine) | 99.5% | 98.2-98.9% | 98.0-98.2% |
| dedicated VRAM (process) | 6.39-6.41 GB | 4.19 GB | 3.04-3.13 GB |

Both modes are GPU-bound on this card (native's render thread waits about 16 ms a frame in `PresentPace`, its submission thread
about 18.8 ms in `WaitPresentedFrames`). The gap went from 1.8-1.9 ms to 0.16-0.36 ms; native's p95 and p99 are now below guest's.
perf-taa is heavy enough here (about 52 fps), so the earlier RX 6600 save was not needed.

**Per stage, GPU ms (native list timestamps, mean of 5 blocks; guest D3D11 event timers `g2gd` and graph timers `g2gt`).** Guest
mappings marked "est." are sums of its events; its Player View self time (1.89 ms) holds the VL generate and raymarch and other work
outside named events.

| Stage | Native before (`g2nt`) | Native after (`g5nt`) | Guest |
|---|---|---|---|
| start | 0.16 | 0.16 | 0.34 (Water Effects' cubemaps) |
| DCLF shadows | 2.33 | 2.36 | 2.26 (event), 2.10 (graph) |
| pre-depth | 0.57 | 0.58 | about 0.5 (est.: terrain shadows 0.04, skylighting probes 0.44 under World) |
| DCLF Z-prepass | 2.98 | 2.99 | 3.12 (Depth event), 2.87 + 0.04 async (graph) |
| pre-main | 1.41 | 1.41 | about 1.2 (est.: Shadowmasks 0.73, SSS raymarch 0.32, terrain blending 0.09) |
| DCLF opaque | 4.08 | 4.13 | 4.08 |
| deferred+water | 3.66 | 3.68 | about 4.0 (est.: World less DCLF 4.80, less the prepasses counted above) |
| DCLF forward | 0.49 | 0.50 | 0.50 |
| post | 4.40 | 2.81 | about 3.1 (est.: image-space events 0.84, outside events 0.40, Player View self 1.89) |
| UI, first person, anchors | 0.07 | 0.07 | 0.03 (first person) |
| sum / graphics span | 20.11 / 20.99 | 18.65 / 19.57 | GPU frame 19.23 |

Native's GPU work is now below guest's GPU frame; what is left of the gap is idle on the graphics queue (span less busy 0.92 ms with
list timing on, against 0.37-0.48 ms on the RTX 5080).

**Fixed: the VL generate in scratch memory (measured).** Native's `cs.volumetric-lighting.generate.0` took 2.20 ms (0.22 ms on the
RTX 5080). Its ISA (`BASICRHI_VK_DUMP_ISA`) had 48 scratch operations a thread: the ORG build made a static array of the three
cascade matrices from the latched block and indexed it per thread, and indexed a local step table, which AMD's compiler keeps in
scratch. The cascade's rows are now read from the latch buffer by offset and the step comes from the index's bits (the same values).
0.64 ms after; post 4.40 to 2.81 ms. CS `f5d75bfab`. A scan of every native pipeline's ISA found scratch in two more: one DCLF
Effect vertex pipeline (1,036 scratch operations; the forward stage costs the same as guest's, so it is not hot here) and Exponential
Height Fog's `VolumetricFogLightScatteringCS` (80; it does not run in this scene). None of guest's 145 dumped pipelines use scratch.

**Fixed: a staging leak of 2.1 GB (measured).** Native held 6.4 GB against guest's 3.1 GB. CS's VRAM ledger (`vram-json`,
`org.vram_source=org`) put 2.2 GB in staging buffers: `cs.bink.staging` 166 x 8 MB, `cs.scaleform.staging` 2,116, `cs.imgui.staging`
1,325, growing by one Scaleform and one ImGui buffer a frame (ReBAR memory, so it counts as VRAM). ORG's recording retention kept
everything a re-recorded inline late batch retained, so each frame's staging buffer stayed until the batch went away. The list's last
submission is done before it records again, so the retention is released there. ORG `f3557aa`, CS `2bd3c4740`. After: 45, 9 and 22
buffers, flat over 300 frames; device-local use 6,260 to 4,147 MiB. At about 0.5 MB a frame the old build would have passed the
6.75 GB budget within minutes of play (estimated); in these 60 s runs it stayed under it, so no frame-time change is attributed to
it. Left against guest: about 1.1 GB (ORG alias pools 275 MB; 414 MB of buffers and textures outside the ledger's owners, among them
the engine's vertex and index buffers and terrain shadows' 72 MB heightmap; not analysed further).

**The user's hypotheses on this card.**
- Pass count (i): not supported. The start stage, with the 129 reflection-sky passes, is 0.16 ms in all (the sky faces' list
  0.10 ms); the UI stage with Scaleform's instances 0.015-0.021 ms. The census counts a native frame at 272 render passes, 73 lists,
  694 image barriers (492 layout changes) and 602 buffer barriers, as on the RTX 5080. The batching experiment was not run: the
  passes it would merge cost too little to move the frame.
- Barriers and layout changes (ii): they cost more here. In per-pass mode the barrier batches inside lists add up to 0.25 ms a frame
  (largest 0.08 ms before the shadow-mask draws, 0.04 before SSGI's depth prefilter, 0.03 + 0.03 at the upscaler's depth copies);
  on the RTX 5080 none reached 0.01 ms. `CS_ORG_KEEP_SKIPPED_BARRIERS=1` changed the span by 0.02 ms.
- Images in the wrong layout (iii): not shown; the census has no GENERAL attachments in native. The handoff below is the suspect.
- Boundaries: a queue submission costs much more on this GPU. Every list in a submission of its own (`BASICRHI_SPLIT_SUBMITS=1`)
  added 0.91 ms of graphics span (about 64 more submissions, about 14 us each; 1.3 us on the RTX 5080). Eight command buffers per
  list (`BASICRHI_PAD_LISTS=8`) added only 0.12 ms of span (about 0.2 us each, as on NVIDIA) but took the frame from 19.6 to 24-28
  ms: on AMD the empty command buffers cost the submission thread, not the GPU. `CS_ORG_MERGE_HANDOFFS=0` with
  `CS_ORG_UPLOADS_WITH_BATCHES=0`: span 19.66 to 19.76 ms.

**Where the GPU idles (submission trace, steady frames, timing on; RTX 5080 values from the third pass).**

| Previous list -> next list | RX 6600 | RTX 5080 | Between them |
|---|---|---|---|
| Scaleform -> next frame's first list | 761 us | 21 us | present relay, present, acquire, handoff (waits on the present and the frame values) |
| post capture.menubg -> Scaleform | 483 us | about 7 us | one handoff list: 14 image barriers, 8 layout changes (untimed) |
| SSS raymarch -> shared upload | 143 us | 16 us | the G-buffer clears (untimed) |
| shared upload 1 -> 2 | 54 us | 31 us | the VL depth copy (untimed) |
| 39 list boundaries inside a submission | 1.2-6 us each | 3.6-5.9 us | list, drain |

The waits these submissions carry were signalled by submissions made 38 to 59 ms earlier, so the CPU is not the cause. The frame
boundary also holds whatever DWM and other processes run on the GPU between frames (the game's window is covered); guest's
equivalent was not measured. The 483 us handoff is most likely a decompression at a layout change (an estimate: handoff lists are
not timed).

**RGP** was not used in this pass: its CLI capture works, but the analysis is GUI-only and decoding the SQTT markers by hand was not
worth the time against the timestamps above.

**Other findings on this card.**
- native-validation reports two lifetime hazards the RTX 5080 runs did not: `VUID-vkDestroyImage-image-01000` (2, an image destroyed
  during the loading screen while an ORG list still used it) and `VUID-vkDestroyImageView-imageView-01026` (8, during gameplay,
  views still in a descriptor of an in-flight list). Not fixed.
- With the VolumetricLighting feature off, native stops at a fatal schedule mismatch: the engine's `ISPerlinNoiseCS` dispatch has
  no CS pass. Not fixed (a feature-off configuration).
- Every launch logs the render thread watchdog's 15 s Present stall during startup (shader and pipeline compiles); a cold guest run
  stopped once on it at the loading screen (`ShaderCompiler::Compile` from DCLF's portal setup) and passed when run again.

**Left, ranked (estimates).**
1. The frame boundary (0.76 ms with timing on), part of it possibly other processes' GPU work.
2. The handoff before Scaleform (0.48 ms): find which layout change decompresses, and keep that image in a layout that needs none.
3. Barrier batches inside lists (0.25 ms) and the untimed G-buffer clears (0.14 ms).
4. Submission boundaries: about 14 us each, 9 a frame (about 0.13 ms).
5. VRAM: native about 1.1 GB above guest.
6. The two lifetime hazards, and DCLF's HZB reading 0 on AMD (occlusion culling stays off there).

**Checks (RX 6600).** Final build (DLL 58268e228901 and the VL shader), one background run each: native-quick, native-river,
native-interior, native-validation, the loading-thread run, the frozen-world run, guest-quick: passed, 0 tap errors, no `NoPass`,
recipe checks 0 findings, no failed build; native-validation shows the known 20 `VUID-vkCmdDraw-None-09600` and back-buffer layout
lines and the 2 `vkDestroyImage` hazards above (the view hazard did not show in this run). Shots match the earlier RX 6600 shots of
this session (means within 1.4) and the RTX 5080's quick shot in content; native and guest shots of the same view differ by the same
amount before and after the changes. ORG's unit tests pass, run one at a time, and the Vulkan host test's modes inline, sync, async,
reuse, revision, segment, external, external-reuse, split, built, stream, arena, rebinding and contributor on the RX 6600; pipelines
timed out once after "input layout build" (where it hung once before on the RTX 5080) and passed when run again (the lease modes
and HeadlessBenchmark not run). The MO2 instance keeps `+CS AB nfg` and `*TerrainHelper.esp`, and every run loaded into the
world on its own.

**Second pass (2026-11-17, the frame's end, barriers, lifetime hazards).** Measured on the RX 6600 as above, background runs with
`DXVK_DEBUG=hang`, the machine in use (VS Code and DWM on the same GPU).

*The 483 us handoff and the 761 us frame boundary were not steady-state costs.* The submission trace analyses seven groups of six
frames, one every 300 frames; the first world group (about frame 600, just after the load, while textures and buffers are still being
made) had gaps of about 2.6 ms before Scaleform and 2 ms at the frame boundary, and the means over all groups carried them. Over the
last five groups only (`subtrace.py --last 5`), in every trace taken (the one of the first pass included):

| Gap | All groups (first pass) | Last 5 groups, 8 traces of this pass |
|---|---|---|
| post `capture.menubg` -> Scaleform (handoff, 14 image barriers, 8 layout changes) | 483 us | 12 to 24 us |
| Scaleform -> next frame's first list (the frame boundary) | 761 us | 331 to 440 us (577 with the lazy acquire below) |

In those settling frames the device made about 1,100 to 2,800 memory allocations per 300 frames (buffers, textures and heaps, 0.6 to
1.3 GiB; BasicRHI now logs them), against 89 (13 MiB) in the last window of a perf-taa run.

*Barrier probe.* `BASICRHI_BARRIER_PROBE=<list label part or *>` with `CS_RHI_CENSUS` records each image barrier of the matching lists
as a command of its own between bottom-of-pipe timestamps; the table (mean us a frame, per list, image, layouts and range) goes to
`BASICRHI_BARRIER_PROBE_FILE` every 300 presents. Handoff lists are now labelled with their batch's first pass (`handoff > <pass>`).
Splitting the barriers adds about 0.5 us of drain each, so the per-barrier numbers are upper bounds. The layout changes over 5 us a
sample in a steady perf-taa frame (all others are 0.5 to 2 us):

| Image (list) | From -> to | us | Cause |
|---|---|---|---|
| main depth copy, `kPOST_ZPREPASS_COPY` (`cs.shadow-mask.world.draws`, first reader after pre-main's depth copy) | TRANSFER_DST -> DEPTH_READ | 81 | a copy into a depth image leaves its HTILE uncompressed; the move to a depth layout re-initialises it (estimate of the mechanism) |
| a depth image written by a copy (`cs.shared.upload.0`, deferred+water) | TRANSFER_DST -> DEPTH_READ | 38 | as above |
| the upscaler's depth copy (`cs.upscaling.copy-to-main`) | TRANSFER_DST -> DEPTH_READ | 27 | as above |
| the upscaler's source (`cs.upscaling.copy-to-main`) | GENERAL -> TRANSFER_SRC | 26 | a storage image read by a copy |
| the upscaler's input (`cs.upscaling.evaluate`) | TRANSFER_SRC -> GENERAL | 18 (x1.5) | back to storage after the copy |
| VL depth copy (`cs.volumetric-lighting.depth-copy`) | DEPTH_READ -> TRANSFER_DST | 12 | depth decompression before a copy |
| main depth (`handoff > cs.engine.skeleton.depth-copy.post-zprepass`) | DEPTH_ATT -> TRANSFER_DST | 11 | as above |
| main depth (`handoff > cs.shared.upload.0`) | DEPTH_READ -> TRANSFER_DST | 9 | as above |
| Cloud Shadows' 32 layer cubes (`handoff > cs.cloud-shadows...reflections-prepass.gather`, then in the list) | SHADER_READ -> GENERAL x33, GENERAL -> GENERAL x95, GENERAL -> COLOR_ATT x32 | 1 to 2 each, about 0.1 to 0.3 ms a frame in all | per-frame layout round trips of every layer cube |

The costly ones are all depth images going through transfer layouts: the depth copies (pre-main's, the VL depth copy, the upscaler's)
are `vkCmdCopyImage`s, about 0.2 ms a frame together. DXVK copies depth with a draw where it can keep compression; that was not tried
here. The 0.08 ms "before the shadow-mask draws" of the first pass is the first of these.

*The frame boundary (steady, 0.33 to 0.44 ms with list timing on).* Between Scaleform's range and the next frame's first range: Scaleform's
two kept lists (one with the present copy into the swap chain image), the present relay, the present, the acquire, and the next
frame's handoff (38 layout changes, 33 of them Cloud Shadows' cubes). Estimated split:
- ours: the next frame's handoff (about 30 to 60 us, probe), five submission boundaries (about 70 us at 14 us each), the present copy
  and the Scaleform lists (about 30 us, not timed): about 0.15 ms;
- not ours: other processes' work on the 3D engine. The Windows `GPU Engine` counters over a steady window gave VS Code 0.7 to 1.4%
  and DWM 0.3 to 0.4% of the 3D engine (about 0.2 to 0.35 ms a frame at 19.5 ms), against Skyrim's own; guest sees the same
  processes. The window could not be uncovered without taking focus, so this was not measured with it uncovered.
- The `System` process's copy engine read 8.4% in one native sample (0.15 to 0.16% in the others and in guest): paging, seen once.

Tried and reverted (no gain outside the spread):
- the acquire made lazily, just before the present copy instead of right after the present (BasicRHI had the acquire's wait batch
  order the whole next frame): boundary 486 to 577 us against 331 to 440 us;
- timeline waits on values the same queue signalled (the Scaleform kept lists' wait on the graph timeline, the relay's) replaced by a
  full barrier: boundary 345 to 380 us against 357 to 411 us (two runs each);
- a list probe (a timestamp command buffer between every list): AMD's bottom-of-pipe timestamp in a command buffer of its own does
  not wait for the previous command buffer's draws, so it misplaced whole lists into the boundaries; removed.

The native present is not the difference to DXVK: both use an A2B10G10R10 swap chain (the Upscaling feature asks for R10G10B10A2 in
SDR), IMMEDIATE, three images; native copies the back buffer into the image, DXVK draws it.

*G-buffer clears and the in-list barriers.* The clears list (`cs.deferred.clears.gbuffer`, 1 pass, 1 memory barrier) is 125 to 141 us
between the SSS raymarch and the next upload in every trace, steady. Not changed: no per-clear timing was taken, and the first pass
on the RTX 5080 showed load-operation clears making later readers slower. The in-list barrier cost is the table above.

*Submissions.* 9 graphics submissions a frame as before; not changed (see "Left").

*Lifetime hazards.*
- `VUID-vkDestroyImageView-imageView-01026` (8 at once, 3 of 6 validation runs before): the stack (now logged with the message) is
  FidelityFX's Vulkan backend under `sl.fsr`, inside CS's upscaler evaluate recorded at execution, freeing the image views of an earlier
  dispatch. It came right after "load hold: started (... the map menu closed ...)": while Drawcall Limit Fix holds presents, the native
  present path returned without presenting and nothing paced the frames, so the CPU ran as many frames ahead as the graph has slots,
  more than FidelityFX keeps views for. Held frames are now paced as a present would pace them (`Native::PaceHeldPresent`: the graphics
  queue's progress the frame latency back). CS `d546f2b42`.
- `VUID-vkDestroyImage-image-01000` (2 per run in 5 of 10 validation runs before, always in the loading screen's load hold): a
  `Native::Texture2D` freed by `RenderGraphRuntime::ServiceD3D11Releases` on the loading screen's thread, which owned rendering then.
  The native release protocol freed an owner once the host's retired submission count passed the count at its release; work on the
  graphics queue the host does not number (the loading screen's thread, external work, the immediate context) was not covered. The
  pacing fix alone did not remove it (it came back once, in the synchronization-validation run). A release now also notes the
  graphics queue's progress and waits for it as well. CS `33a65a65a`. Which list held the image was not named: the command buffer's
  label lookup (new in BasicRHI) found nothing for it, so the exact submitter is inferred, not shown.
- After both fixes: 3 validation runs (one with `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1`) with only the known 20
  `VUID-vkCmdDraw-None-09600` and back-buffer layout lines, no destroy hazard, no `SYNC-HAZARD`.
- ORG `f3557aa`'s claim (an inline late batch's last submission has completed before it records again) holds on the paths read:
  `GraphHost::TakeLiveCopy` hands out a live copy only once its lease is retired (its queue points reached), and revision-driven copies
  wait for their points (`TakeCopyLease`).

*Volumetric Lighting off.* With `features.VolumetricLighting=false` and `CS_ORG_MISMATCH_FATAL=0`, native logs `ISPerlinNoiseCS` once
and then about 3 more "no CS pass" errors every world frame (138 in 46 frames): the whole vanilla volumetric lighting (noise, generate,
raymarch, apply, blur) has no pass, because the ORG volumetric lighting client and its detours are installed only by the feature. Not
fixed: a noise pass alone would only move the stop to the generate. The likely fix is to install the client in native mode whatever the
feature's state, with the shaders built without the feature's defines, and to check its output against guest with the feature off.

*Scratch memory.* `VolumetricFogLightScatteringCS`: the cascade matrix is read from the structured buffer by index and the engine
local shadows from the latch by offset; the RX 6600's ISA has no scratch operation left (80 before). CS `937686330`. DCLF's Effect
vertex pipeline still has 1,036 (`graphics_Effect__main_`); not changed.

*VRAM.* Not changed. The device's allocation report shows the settling churn above; the 1.1 GB against guest stays unattributed beyond
the first pass.

*Before and after (Tracy, 6 s windows, alternating runs in one session; timing off).* No change here touches a steady world frame (the
pacing and the release gate act only in held frames and at releases; the probes are off by default), and none was expected.

| | Native first pass (`b3n*`) | Native after (`b4n*`) | Guest (`b4g*`) |
|---|---|---|---|
| frame period, mean | 19.66 / 19.49 ms | 19.47 / 19.48 ms | 19.24 / 19.26 ms |
| p50 / p95 / p99 | 19.44-19.55 / 20.56-20.58 / 21.4-21.7 ms | 19.38-19.44 / 20.60-20.72 / 21.6-21.8 ms | 18.92-19.12 / 22.25-22.28 / 24.1-25.0 ms |
| GPU busy, Skyrim's 3D engine / all processes | 98.2-98.9% | 98.6-99.0% / 99.3-99.6% | 98.0-98.1% / 98.8-98.9% |
| dedicated VRAM (process) | 4.19 GB | 4.19 GB | 3.03 GB |
| graphics submissions a frame | 9 | 9 | - |
| idle at the frame boundary, list timing on, steady | (761 us, all groups) | 331 to 440 us | - |

Native trails guest by 0.21 to 0.23 ms (1.1 to 1.2%), outside the 0.02 ms spread of each mode in this session; native's p95 and p99
stay below guest's.

*Checks (second pass).* Final build (DLL 89db065ea036), background runs: native-quick (twice), native-river, native-interior, the
loading-thread run (twice), the frozen-world run, first and third person, guest-quick: passed, no `NoPass`, recipe checks 0 findings,
no failed build; every log names the RX 6600. native-validation three times on the final build (one with synchronization
validation): only the known 20 `VUID-vkCmdDraw-None-09600` and back-buffer layout lines, no destroy hazard, no `SYNC-HAZARD`. The quick
run's shot matches the first pass's (106.5 against 106.8 mean). Volumetric Lighting off: still stops (see above). ORG's unit tests pass
run one at a time (AliasPacking, AsyncCompile, ContributorRegistry, ContributorHeaderC, DeviceRegistry, GoldenSchedule, Persistent,
Rebinding, Recording, ResourceUse, Token, TrackingWorld) and the Vulkan host test's modes inline, sync, async, reuse, revision, segment,
external, external-reuse, split, built, stream, arena, rebinding and contributor. `pipelines` hung (Debug, no ORG change in that area):
the main thread waits in `org::PipelineLibrary::Wait` from `TestPermutations` while every thread-pool worker idles in its condition
variable, so a permutation's completion was never signalled or its job never queued (stacks from a procdump; HeadlessBenchmark and the
lease modes not run). The MO2 instance keeps `+CS AB nfg`, `+Start On Save` and `*TerrainHelper.esp`, and every run loaded into the
world on its own.

*Left, ranked (estimates).*
1. The depth copies through transfer layouts (about 0.2 ms a frame of layout changes, table above): copy depth with a draw, as DXVK
   does, or keep the copies' targets out of transfer layouts.
2. The frame boundary's own part (about 0.15 ms): the next frame's handoff (Cloud Shadows' cube round trips), five submission
   boundaries, the present copy.
3. Cloud Shadows' per-frame layout round trips of its 32 layer cubes (0.1 to 0.3 ms with the probe's overhead): keep the cubes in
   one layout across the frame.
4. The G-buffer clears list (0.13 ms), not split by clear.
5. Volumetric Lighting off in native (a stop, not a cost).
6. DCLF's Effect vertex pipeline in scratch memory (1,036 operations; the forward stage costs the same as guest's).
7. VRAM: about 1.1 GB over guest, and the settling frames' allocation churn.

### VRAM, native against guest (2026-11-17)

Measured on the RX 6600 as above: perf-taa at 1920x1080, background runs with `DXVK_DEBUG=hang`, VRAM Budget off in both modes.
"Process" is Windows' `GPU Process Memory` dedicated usage for Skyrim's process, sampled six times from the start mark (MiB).
It reads about 45 MiB above the driver's device-local heap usage (`VK_EXT_memory_budget`) in both modes; the table sums to the
driver's figure.

**How it was read.** Guest: CS's VRAM ledger (`vram-json`, `cs.vram_debug`, `org.vram_source=org`), which lists DXVK's D3D11
resources by owner and ORG's allocations. Native: that ledger counts only ORG's allocator blocks, so the 1.5 GiB of committed
textures fell into its "not accounted" line. BasicRHI now keeps a ledger of every live `vkAllocateMemory` by site and kind
(device-local, ReBAR, system), logged with the allocation report every 300 presents. `BASICRHI_MEMORY_DUMP=<file>` writes every
live allocation, and to `<file>.resources.csv` every live resource with its memory, offset and name, so the heaps' contents are named
and aliased placements show as overlaps. `BASICRHI_COMMAND_POOL_SITES=1` logs the live command pools by the call stack that made
them (resolve the offsets with `llvm-symbolizer --obj=build/Perf-Fast/CommunityShaders.dll --relative-address`). The driver's own
memory is the heap usage less every live allocation. `VK_EXT_device_memory_report` (`BASICRHI_DEVICE_MEMORY_REPORT=1`,
`DXVK_DEVICE_MEMORY_REPORT=1`) is not offered by this driver.

**By category (MiB of driver device-local usage; measured, except that a few resources were put in a category by their name).**

| Category | Native before | Guest before | Native after | Guest after |
|---|---|---|---|---|
| Driver's own memory (command pools, pipelines, query pools, swap chain) | 972 | 575 | 582 | 572 |
| Engine render targets, with the Deferred G-buffer and HDR's copies | 562 | 377 | 562 | 377 |
| SSGI, SSS, volumetric lighting and image-space transients and history | 482 | 145 | 432 | 145 |
| Terrain Shadows | 216 | 49 | 216 | 49 |
| Staging and upload (ReBAR) | 89 | 0 | 81 | 5 |
| Scaleform scratch images, Bink's movie texture | 41 | 0 | 41 | 0 |
| ORG frame arena | 28 | 0 | 29 | 0 |
| Game textures | 740 | 750 | 735 | 750 |
| Engine shadow maps | 330 | 330 | 330 | 330 |
| Engine vertex and index buffers | 99 | 108 | 99 | 108 |
| DCLF buffers | 117 | 146 | 107 | 146 |
| Allocator slack, free space in DXVK's chunks | 218 | 248 | 240 | 341 |
| Descriptor heaps, DXVK internal, ORG unlisted | 31 | 53 | 31 | 53 |
| Other CS features (Skylighting, Terrain Blending, Dynamic Cubemaps, ...) | 214 | 218 | 213 | 218 |
| Sum (driver device-local usage) | 4,137 | 2,999 | 3,697 | 3,096 |
| Process dedicated (Windows) | 4,189 | 3,033 | 3,686 to 3,754 | 3,032 to 3,131 |

Guest's two runs differ only by one more DXVK chunk (free space, 94 MiB). In native, the "engine render targets" row holds engine
targets that no world frame writes, about 125 MiB; guest's DXVK never allocates those.

**Fixed.**
- *Command pools (measured: about 340 MiB).* Each `VkCommandPool` costs this driver about 2 MiB. Native had 226: 32 for the
  immediate context's slots and 64 for the out-of-graph commands' slots (one allocator each), and about 120 in ORG's pools (every
  epoch graph had pools of its own, each keeping at least one allocator, and a batch claimed one allocator per list). The slots now
  share one allocator per context (Vulkan; D3D12 keeps one per slot): 4,189 to 4,000. ORG's graphs on the host's timelines share the
  device's pool for each queue kind, a batch begins its lists under one claim (`CommandListPool::BeginGroup`), and the background
  warm-up makes no new allocator: 132 pools to 19, about 4,030 to 3,880. ORG `8596c63`, CS `b73cf0f07`.
- *Recordings of replaced builds (measured: 132 MiB).* A CS stage keeps the recordings of its last 24 descriptions. A recording of a
  replaced build is never used again, but it kept that build's graph alive, with its private alias pools (three copies of the SSGI
  and SSS transient pool after a load). A new request now drops them. 3,879 to 3,747. CS `a16f37081`.
- BasicRHI's ledger and dumps: BasicRHI `070abc4`.

**Frame time (same session).** Native perf-taa under a Tracy capture, as the baseline was taken: 18.18 ms mean over 332 frames,
against 19.47 ms before. Without Tracy, between the marks: native 16.49 and 17.12 ms (16.53 with `CS_ORG_EPOCH_STATS=1`), guest
19.15 ms. Skyrim's 3D engine stays 99.4 to 100% busy, so the GPU work itself got shorter. Which fix did it (fewer command pools and
command buffers, 1,636 to 955, or the replaced builds) was not isolated. native-quick and guest-quick pass; their shots match the
earlier ones (105.7 against 106.1, 107.1 against 105.8 mean).

*Correction (2026-11-17, later).* The native periods in this paragraph (16.49, 16.53, 17.12 ms) were taken with a frozen post chain:
CS `a16f37081`, the second fix above, kept the post stage's staged build from switching in after the TAA switch, so the chain drew
nothing and the screen kept one frame ("Aliasing survey", first item). Neither fix made the GPU work shorter. Re-taken on the fixed
build: native 19.02 to 19.08 ms, guest 18.98 to 19.10 ms ("Frozen post chain: re-taken numbers" below). The VRAM figures here stand: a
frozen chain does not change what is allocated.

**Left (native about 600 MiB over guest), with what each would take.**
- Terrain Shadows (+167): ORG makes its textures once, at the largest installed heightmap's size (6144x6144: `ShadowHeight` R16G16,
  144 MiB, and the heightmap as R16, 72 MiB). Guest makes them for the current worldspace (3808x3008: 43.7 MiB, and the heightmap
  kept BC4, 5.5 MiB). Keeping the heightmap in its DDS format would save 54 MiB (the upload must then copy block rows). Sizing to the
  current worldspace needs a rebuild at a worldspace change, which the "rebuilds during play" rule keeps out.
- Bink's staging (64 of the 81): eight 8 MiB blocks stay after the menu movie ends. The UI stage's kept recordings retain the last
  frames (`PassRecordContext::Retain`), and nothing records that pass again. Freeing the pool's spares alone did not help.
- Image-space history and transients in two copies (part of the +287): `ImageSpace::SAO`, `SAORawAO0/1`, `TAAHistory0/1`,
  `SAOBlurTemp`, the SSGI and SSS transient pool and `VolumetricLighting::Volume` each exist twice, in two graphs' private alias
  pools (the stages' private arenas, `SetPrivateAliasEpochs`). If two graphs each keep a history copy, they may also disagree about
  its contents: not checked.
- ORG frame arena (29), Scaleform and ImGui staging (17): not analysed.
- Engine targets no world frame writes (about 125): DXVK allocates an image's memory at its first use; native allocates at creation.

**Candidates for the aliasing piece (MiB, measured).**

| Resources | Size | Why they could alias |
|---|---|---|
| `kBOOK_TEXT` depth and `kBOOK_TEXT_0..3` | 27.3 + 17.8 | written only in the book menu; or allocated at first use |
| `kWATER_1/2` (R16G16B16A16) | 31.6 | never written in this scene; guest has none |
| `kGETHIT_BUFFER`, `kGETHIT_BLURSWAP`, `kMENUBG`, `kSCREENSHOT` | 31.6 | effects and menus only |
| `kLOCAL_MAP/_SWAP`, `kWATER_DISPLACEMENT/_SWAP`, the lens-flare targets | about 11 | map menu, water, lens flares only |
| Engine `kSAO`, `kSAO_RAWAO`, `kSAO_RAWAO_PREVIOUS`, `kSAO_TEMP_BLUR`, `kTEMPORAL_AA_ACCUMULATION_1/2` | 47 | the image-space port draws into its own copies, of the same sizes and formats |
| The port's own `ImageSpace::SAO*`, `TAAHistory*`, `SAOBlurTemp`, `Hdr/LdrScratch` (second copies) | about 70 | one copy could serve the post and menu UI graphs |
| SSGI and SSS transient pool, second copy | 49.5 | one arena shared by the stages (cost about 15 us a submission when measured before) |
| Engine `VOLUMETRIC_LIGHTING_ACCUMULATION` and `_COPY`, next to ORG's `VolumetricLighting::Volume` (two copies) | 22.5 + 22.5 | the ORG client draws the volumetric lighting; guest's feature replaces the engine's |
| `cs.scaleform.scratch.0..3` | 31.6 | GFx filters in menus; the UI stage runs after post |
| Deferred's G-buffer (`cs.native.texture.270..275`) | 46 | written and read within the frame |

### VRAM, second pass (2026-11-17)

Same measurement as above (RX 6600, perf-taa 1920x1080, background, `DXVK_DEBUG=hang`, VRAM Budget off). Native's table comes
from BasicRHI's dump, guest's from CS's VRAM json; both by `cat.py`, which files resources by name (MiB of driver device-local
usage).

| Category | Native | Guest | Native minus guest |
|---|---|---|---|
| Driver's own memory (no vkAllocateMemory) | 586 | 562 | +23 |
| Engine render targets (with Deferred's and HDR's) | 531 | 237 + 75 + 65 | +154 |
| ORG alias pools (SSGI, SSS, VL, image-space transients) | 202 | 0 | +202 |
| ORG image-space port targets (SAO, TAA history, scratch) | 117 | 0 | +117 |
| Scaleform scratch, Bink's movie texture, Scaleform and ImGui staging | 41 | 0 | +41 |
| ORG frame arena | 28 | 0 | +28 |
| Terrain Shadows | 49 | 49 | 0 |
| Game textures | 734 | 750 | -16 |
| Engine shadow maps | 330 | 330 | 0 |
| Engine vertex and index buffers | 99 | 108 | -8 |
| DCLF buffers | 107 | 155 | -49 |
| Allocator slack, free in DXVK's chunks | 255 | 333 | -77 |
| Other CS features, descriptor heaps, DXVK internal | 386 | 437 | -51 |
| Sum (driver device-local usage) | 3,465 | 3,079 | +386 |
| Process dedicated (Windows), with the VRAM debug dumps | 3,514 | 3,121 | +393 |
| Process dedicated, a plain perf-taa run | 3,447 | | |

Before this pass the sums were 3,697 and 3,096, and process dedicated 3,686 to 3,754 against 3,032 to 3,131.

**Fixed.**
- *Terrain Shadows (measured: 216 to 49).* The ORG path's textures were made once at the largest installed heightmap. Now the
  heightmap texture is made per heightmap, at its size and in its DDS format (BC4 kept; the staged upload copies block rows), in a
  client registration of its own, and the shadow height map is a versioned twin that each heightmap resizes as a new version
  (Drawcall Limit Fix's graphs follow the slot with no build). A worldspace change registers the client again, so the EarlyPrepass
  graphs rebuild, staged. The resize must wait for a frame boundary: the first build resized at EarlyPrepass and failed fatally
  ("a slot of its graph published a new version after the frame's Begin"). `Driver::AtNextBegin` runs such work in the next
  `OnBegin`, before the schedules are chosen. Until a full refresh was committed in a graph that holds the new registration,
  `NeedsRefresh` keeps the feature asking for refreshes and keeps terrain shadows off, so no frame reads the new version unwritten.
  CS `f8cee2525`.
- *Bink staging (measured: 72 to 0).* The UI stage's kept recordings retained the movie's last frames: an inline late batch keeps
  what it retained until it is recorded again, and the movie frames' descriptions never come back. The returned blocks then stayed
  as the pool's spares. At a movie's end the stages that hold the Present anchor drop their finished kept recordings
  (`FrameAnchors::DropKeptRecordings`; the slots' schedules keep the ones they run on), and the staging pool lives per movie. CS
  `15504655a`.
- *CS's VRAM ledger (item 5).* It reads BasicRHI's live ledger (`Device::LiveMemoryStats`, BasicRHI `270ade8`) in every mode,
  listed by site and kind; CS's own `vkAllocateMemory` hook is gone. Native's "not accounted" line is now the driver's own memory
  (586 MiB); the committed textures (about 1.5 GiB) had landed there. CS `575133931`.

**Attributed, not changed.**
- Scaleform scratch (31.6): four 1920x1080 RGBA8 images, declared by every Scaleform pass of the UI stages, so none is unused by a
  built graph. The aliasing piece's.
- Bink's movie texture (9.0): one for the session, so a movie never rebuilds the Present graph. Freeing it between movies would
  rebuild at the next movie's start, and its first frames would be lost.
- ORG frame arena (27.9): 70 pinned chunks with 22.7 MiB live in 564 regions, the latch regions of the published copies (52 copy
  leases). The rest (about 5 MiB) is the bump allocator's holes: a pinned chunk is reused only once it is wholly empty.
- Scaleform staging (6.8, 27 blocks) and ImGui staging (2.2, 9 blocks): the pools keep up to frames in flight times runs a frame
  spares, and kept recordings of descriptions that do not come back retain their last blocks, as Bink's did. The general fix is in
  ORG: a late batch recorded at every submission could drop what it retained once that submission completes. Not done: whether
  the completion can race the next submission's recording was not established.

**Not done: engine targets' memory at first use (about 125).** In native, the stage graphs declare (and put barriers on) many of
those targets every frame whether or not anything draws: the UI stage's Scaleform passes declare `kBOOK_TEXT_0..3` and its depth,
DCLF's skeleton declares `kWATER_1/2`, the start stage the water displacement pair, the menu UI stage `kMENUBG` and
`kLOCAL_MAP_SWAP` (from the recipe log). Memory bound at the first declaration would save nothing for them. The world recipe never
declares `kIMAGESPACE_TEMP_COPY/2`, `kTEMPORAL_AA_UI_ACCUMULATION_1/2`, `kGETHIT_*`, `kSCREENSHOT` and `kLOCAL_MAP` (about 87 MiB),
nor the engine `kSAO*`, `kTEMPORAL_AA_ACCUMULATION_*` and volumetric lighting targets the aliasing piece owns (about 83 MiB). What it
would take: images whose memory is bound later with their views made now (Vulkan sparse binding, D3D12 reserved resources) in
BasicRHI; the creation-time layout and clear moved to the binding; and a binding at the first native context operation on the
texture or the first graph build that declares its slot. The paths that would bind them (the book and map menus, a hit, a
screenshot) cannot be reached by a background run without input, so they could not be checked here.

**Checks (build `10dea1066fb0`).** native-quick, guest-quick, native-river, native-interior passed; the river shots match the
earlier native run (river-east mean channel difference 3.1). A run that goes Tamriel, Solstheim, Riverwood (`cow
DLC2SolstheimWorld 40 -10`, then `coc Riverwood`) passed in both modes: the shadow height map went 3808x3008, 6144x6144, 3808x3008
as versions, each registration's graphs switched in, no tap error. Its Riverwood shots against guest differ by exposure only (known).
Native perf-taa between the marks: 16.43 ms.

*Correction (2026-11-17, later).* That 16.43 ms was taken with a frozen post chain (CS `a16f37081`, fixed in `811e509a0`); the real
period is about 19.05 ms, the same as guest's.

**For the aliasing piece.** Native's ORG alias pools are 202 MiB and the image-space port's own targets 117 MiB, against guest's
none; the engine targets the port duplicates are still allocated (above).

### RX 6600, third pass (2026-11-17)

Measured on the RX 6600 as above: perf-taa at 1920x1080, uncapped, background runs with `DXVK_DEBUG=hang`, VRAM Budget off. "Period"
is the frame period between the start and end marks (900 frames), no Tracy. Native and guest alternate in one session.

**Step 0: why native got 2 ms faster than guest.** The VRAM pass's numbers held: native 17.04 ms, guest 19.05 ms, both 98.6 to
99.5% busy. The GPU work itself did not change:
- Per stage, with list timestamps (`CS_GPU_EVENT_TIMERS=300`), native summed 18.68 ms against 18.78 ms in the morning's `g5nt`, every
  stage and every list within 0.01 to 0.04 ms. The graphics span with timing on was 19.40 ms against 19.57.
- The census of a steady frame has the same 71 lists, 505 colour and 100 depth attachments and 677 image barriers as before. It counts
  32 render passes fewer: five lists recorded at execution (the post chain, SSR's downsample, the refraction copy, the VL composite,
  the image-space frame clears) now count no pass begin. Their list times are unchanged (SSR's downsample 0.97 against 0.98 ms), so
  this is the census missing them, not missing work. Not fixed.
- Shots: the geometry matches guest's at the perf-taa view. Native's perf-view shots are darker in most runs (means 40 to 66, once
  106; guest 104 every time); the quick shots at world-ready + 60 match guest (106). Likely the known exposure variance, but one-sided
  at this view; not investigated.
- The cause is the shared command pools (ORG `8596c63`). A temporary switch that gave every graph its own pools and every list its own
  allocator again took native to 19.31 ms (16.51 without it, same build, alternating). Either half alone did nothing (16.48 and 16.75
  ms). So on this driver the lists of a frame submitted from many command pools cost about 2.8 ms of GPU time (about 38 us a list over
  about 73 lists); from a few pools they do not. The mechanism inside the driver is not known (estimate: command buffers from one pool
  are chained, from different pools each starts and ends with a flush). The morning's 19.47 ms was this cost, not Tracy: Tracy now
  reads 17.07 ms, the same as without it.
- List timing hides it: each timed list ends with a drain, so with timing on the period is 19.4 ms in both builds. The morning's per
  stage comparison with guest (native about equal) was inflated by those drains, about 2.2 ms. Native's GPU work is about 2.5 ms
  below guest's in this scene; guest's D3D11 timers do not drain and its period matches its GPU frame.

*Correction (2026-11-17, later): step 0 is wrong.* Every native period in this pass (17.04, 16.51, 16.78, 16.48, 16.75, 16.50, 16.33
ms) was taken with a frozen post chain (CS `a16f37081`, fixed in `811e509a0`; "Aliasing survey", first item). Native is not 2 ms faster
than guest, and the darker perf-view shots were the frozen frame. The shared command pools (ORG `8596c63`) do not change the frame
period: the A/B re-taken on the fixed build with the same temporary switch (each graph its own pool, each list its own claim; 53 command
pools against 19) read 19.06 and 19.02 ms against 19.08 and 19.02 ms without it, alternating. What they save is VRAM: process dedicated
3,277 and 3,283 MiB with own pools against 3,254 to 3,261 MiB, about 25 MiB. Why the old A/B read 19.31 ms with the switch is not
known (estimate: the slower command pools let the staged post build switch in before its recordings were dropped, so those runs drew
the chain; not checked). With list timing on
the graphics span is 19.22 ms, the period without timing 19.02 to 19.08: list timing's drains cost about 0.15 ms, not 2.2, and native's
GPU work is about guest's in this scene, not 2.5 ms below it. Re-taken numbers: "Frozen post chain: re-taken numbers" below.

The barrier probe (`BASICRHI_BARRIER_PROBE=*`) was re-taken first. Its floor is the drain it adds, 0.5 us a barrier.

**1. Depth copies through transfer layouts (done).** The depth layout changes around the copies took 211 us a frame in the probe
(`kPOST_ZPREPASS_COPY` TRANSFER_DST -> DEPTH_READ 118 us in the shadow-mask list, the DeferredPasses copy 38, the upscaler's 27, the
VL copy 12, two handoffs 17). An `AnchorPasses::Copy` between two depth textures of one format is now a fullscreen draw
(`RenderGraph/DepthCopy.hlsl`): the source read through its depth and stencil views, the target the depth attachment with an Always
test, the stencil written through `SV_StencilRef`. No image leaves a depth layout. Where the device cannot write stencil from a shader
(NVIDIA's D3D12), a depth-stencil copy stays a transfer copy.
- BasicRHI: a depth-stencil SRV of plane 1 reads the stencil (Vulkan's stencil aspect; D3D12's X24G8 or X32G8X24 format), and
  `FixedFunctionFeatureInfo::stencilExport` (Vulkan enables `VK_EXT_shader_stencil_export`). BasicRHI `4d819d4`.
- ORG: a 2D depth-stencil texture also gets `SRVViewType::Texture2DStencil`. ORG `dd53ed9`. CS `88f303838`.
- After: none of those layout changes is left in the probe. List timestamps: shadow-mask list 0.26 to 0.17 ms, DeferredPasses' upload
  list 1.70 to 1.67, the upscaler's copy list 0.18 to 0.15, the depth-copy list 0.17 to 0.15; stages' sum 18.68 to 18.57 ms, graphics
  span 19.40 to 19.24 ms (measured). In the frame period this is inside the spread.
- Left: the upscaler's depth source sits in GENERAL and goes GENERAL -> DEPTH_READ -> GENERAL around the copy (26 + 20 us); it used to
  go through TRANSFER_SRC the same way. Not changed.
- *Re-read with the post chain running (2026-11-17, later).* The claim holds: the probe has no depth layout change through a transfer
  layout around these copies, and the list timestamps read shadow-mask 0.17 ms, DeferredPasses' upload 1.67, the upscaler's copy 0.15,
  the depth copy 0.15; stages' sum 18.52 ms, graphics span 19.22 ms.

**2. Cloud Shadows' layer cubes (dropped).** Their 290 or so layout changes a frame read 0.55 to 0.78 us each in the probe, against the
probe's 0.5 us drain. The list that holds 95 of them (`reflections-prepass.gather`) is 0.04 ms in all with list timing. About 0.05 ms
a frame in all (estimated), below what a run can show. Not changed.

**3. The G-buffer clears (measured, not changed).** With a temporary mask of which targets the clears list clears, the gap it leaves
in the submission trace was 130 us with all six, 4.3 us with none and 31 us with NormalRoughness alone: about 25 us a 1920x1080 32-bit
target. Each clear writes its whole target (about 330 GB/s); none is a fast clear. A faster clear would need these images to be
compressed by the driver in a way that allows fast clears (their creation flags and the clear colour); not done.

**4. The frame boundary (analysed, not changed).** It is 331 us with timing on: Scaleform's submission (its two lists, one with the
present copy), the present relay, the present, the acquire, and the next frame's handoff. Two of the five boundaries are the present
and the acquire. The other submissions inside the frame are epoch boundaries, and merging them means holding submissions back, which is
ruled out. What is left is folding the relay into the last graph submission (one submission, about 14 us), which needs the swap
chain's semaphore passed down to that submission. Not done.

**5. DCLF's Effect vertex pipeline in scratch memory (done).** The ORG build of `Effect.hlsl` copied the draw's indexed texture
coordinate row into a `static float4 IndexedTexCoord[128]` at the vertex's index and read it back there. The row is now read from the
latch where it is used (`EffectIndexedTexCoord`). An ISA dump of a native quick run has no scratch instruction in any of its 3,561
pipelines and no 128-entry indexed array. CS `0057a2193`.

**6. DCLF's HZB on AMD (fixed).** `HzbTest` returned early on `a_view.Index == 0 || a_view.Mips == 0`: the RDNA2 miscompile in
AGENTS.md, which folds `Index` to 0 past the guard, so the taps read heap slot 0. Now one unsigned relational compare. With
occlusion forced on, perf-taa reads 0 of 2,693 footprints all-near (before: nearly all), about 460 draws a window are occluded, phase 2
brings back 19, and the mid field is whole. Occlusion is the default cull mode on AMD again (the AMD exception and the saved-setting
check built on it are removed). CS `f6566f342`.
*Re-checked with the post chain running (2026-11-17, later):* the default stands. Native perf-taa with occlusion read 19.02 to 19.08
ms, with `dclf.cull=frustum` 19.10 and 19.14 ms: about 0.05 ms gained, inside the spread. The perf shots with and without occlusion
have the same means (91.6, 112.0, 108.7) and differ only by animation (mean difference 3.5); the mid field is whole in both.

**Native against guest, final build (DLL 55e5af235c20; occlusion on in both).** *Correction (2026-11-17, later): the native periods in
this table and the line below it were taken with a frozen post chain; the native VRAM was before the aliasing pass. The re-taken table
is in "Frozen post chain: re-taken numbers" below.*

| | Native | Guest |
|---|---|---|
| period, run 1 / run 2 | 16.50 / 16.33 ms | 19.01 / 19.06 ms |
| Skyrim's 3D engine busy | 99.6 / 99.6% | 98.5 / 98.0% |
| process dedicated VRAM | 3,447 / 3,395 MiB | 3,035 / 3,043 MiB |

Native's periods at the start of this pass (same session, before these changes): 17.04, 16.51, 16.78, 16.48 and 16.75 ms.

**Checks.** Build with items 1, 5 and 6 (DLL 3570c64034a3), one background run each: native-quick, native-river, native-interior,
guest-quick passed, recipe checks 0 findings, no `NoPass`, no failed build, no device loss; native-validation shows only the known 20
`VUID-vkCmdDraw-None-09600` and back-buffer layout lines. The final DLL (same sources) passed native-quick. The quick shot matches the
morning's (105.9 against 105.8 mean).

**Found and not fixed: native D3D12 on the RX 6600 loses the device** about 10 s into the world (`0x887A0006`, a page fault at an
address in no allocation, DRED's first incomplete list unnamed). It was never run on this card before (all earlier native-d3d12 runs
were on the RTX 5080). It happens with the depth-copy draw, the stencil views and occlusion all switched off, so it is not from this
pass.

## Aliasing survey (2026-11-17)

RX 6600, perf-taa at 1920x1080, native Vulkan, background runs with `DXVK_DEBUG=hang`, VRAM Budget off. Sources: BasicRHI's memory dump
(`BASICRHI_MEMORY_DUMP`, every live resource with its memory and offset) and, from the same run, every attached epoch graph's resources
(`CS_ORG_GRAPH_RESOURCES_FRAME=<n>`, new: one log line per graph, as its schedule names them). Run `g1n` is the state before this pass
(with the post fix below), `g5n` after. Game textures, DCLF's buffers under 1 MiB and resources under 0.25 MiB are left out.

### First: the post stage froze after TAA was switched on (fixed)

Native's perf-taa shots were darker than guest's because the screen was frozen. Since CS `a16f37081` (drop recordings of older builds)
a new recording request dropped every finished recording not on the attached build, the staged build's too. With TAA the post stage's
description alternates by frame (its history rows by slot parity), so it asks again every frame, and its staged build never had all four
slots recorded. The post stage stayed on the build from before the TAA switch, whose chain client was no longer tappable: the chain drew
nothing from then on, and the final target kept the last frame drawn (two shots 900 frames apart were pixel-identical). The darkness was
that frozen frame, taken mid-adaptation after the load; it varied by run. Adaptation itself was fine, and it never existed twice.

Fixed in CS `811e509a0`: the staged build's recordings are kept. perf-taa's shot now matches guest's (104.5 against 104.5 mean).

**This changes the frame period.** Native perf-taa between the marks is now 19.07 ms, guest 19.11 ms (same session, one run each). The
16.3 to 17.1 ms native periods of the VRAM passes and the RX 6600 third pass were measured on builds with `a16f37081`, so with no post
chain after the TAA switch: native is not 2 ms faster than guest in this scene. The command-pool A/B in the third pass (19.31 against
16.51 ms) was probably the same artifact and should be taken again. Re-taken: shared pools do not change the period (below).

### Frozen post chain: why it was silent, and the guards (2026-11-17)

Why nothing failed: every chain step that its schedule left out went through `NoteLaggedWork`, which only counts. Lagged work is
allowed while a build or recording is on its way, but nothing bounded it, so a staged build that never switched in kept the stage on
the old build for good, its chain steps lagging every frame. The shots were compared only by eye.

Now:
- **Starved staged build (fatal in development builds, logged in release).** `FrameAnchors::Schedule` counts, per staged build, the
  recordings asked for again after a finished one was dropped. Past 16 (four a frame slot) it is `TapError::Starved`: the build would
  never switch in. A count, not a timer; the census line has the highest count per stage ("staged recordings asked again").
- **Untappable client (same policy).** A chain, water or world step whose client is not in the build its stage runs goes through
  `Driver::NoteStepLag`. It lags while `FrameAnchors::TapMayLag` finds something on its way: a build holding the client's registration,
  a staged build, a key build (absent passes), a lagging or absent schedule. The first frame the engine draws a pass the build left out
  also lags (its request makes the next Begin ask for the key's build). Nothing on its way in the next frame is `TapError::Untappable`.
- **cs-test's live-image check.** `base.toml` sets `expect.live_frames = 30`: at the exit, in the world phase, the game takes two shots
  30 frames apart, and the harness fails a run whose two have the same pixels (testing.md).

Proof, on a build with a temporary switch that reverted `811e509a0` (not committed): native perf-taa stopped at frame 178 (the loading
screen) with "staged build starved" in the deferred+water stage (20 recordings asked again). With `CS_ORG_MISMATCH_FATAL=0` the run went
on, its period read 16.61 ms, and cs-test failed it: "the final image did not change in 30 frames ... a frozen final target". A fatal
mismatch reaches the harness as `hang` (the failure's message box stops the render thread); the reason is in the log's `[C]` line.

### Frozen post chain: re-taken numbers (2026-11-17)

RX 6600, perf-taa at 1920x1080, uncapped, background runs with `DXVK_DEBUG=hang`, VRAM Budget off, alternating in one session; build
with the guards (DLL `d151eedae364`, the temporary A/B switches in it but off). Period between the marks (900 frames); GPU busy is
Skyrim's 3D engine utilisation and VRAM the process dedicated usage, both sampled six times from the start mark.

| | Native | Guest |
|---|---|---|
| period | 19.08 / 19.02 / 19.06 ms | 19.10 / 18.98 ms |
| Skyrim's 3D engine busy | 99.1 / 99.5 / 99.4% | 98.4 / 98.3% |
| process dedicated VRAM | 3,261 / 3,254 / 3,260 MiB | 3,077 / 3,035 MiB |

- Shared command pools (ORG `8596c63`), temporary switch on: 19.06 / 19.02 ms, 3,277 / 3,283 MiB, 53 command pools against 19. No frame
  period effect; about 25 MiB of VRAM.
- Occlusion off (`dclf.cull=frustum`): 19.14 / 19.10 ms; the default (occlusion) gains about 0.05 ms, inside the spread.
- List timing on (`cs.gpu_event_timers=300`): 19.19 ms period, graphics span 19.22 ms, stages' sum 18.52 ms. The barrier probe run:
  19.67 ms.

### Second: every image-space and SSGI image existed two or three times (fixed)

Not two graphs' private pools, as the VRAM passes guessed. The graph log showed each of these images used by one stage only
(`ImageSpace::SAO*` by deferred+water, `TAAHistory*`, `Hdr/LdrScratch`, `Dof*`, `Bloom*`, `VolumetricLighting::Volume` by post, SSGI and
SSS by deferred+water). The copies were older generations:
- the image-space port made its targets again at every registration (each TAA switch, size or ratio change). Builds still alive held
  the old ones: the replaced build's kept recordings (dropped only at the next request, which in a steady stage never comes), and graphs
  outside the frame's kind;
- each held build kept its private alias pool, with its own placement of every transient it used (the second SSGI and SSS pool, 62.7
  MiB, was the replaced deferred+water build's);
- a history made again started over: TAA, SAO and the adaptation reset at each registration, and the new adapted luminance pair
  rebuilt every graph ("published resource replaced").

Fixed (CS `9ffda6521`, ORG `fddf84c`):
- **One owner above the stages.** `Frame::Resources` (`src/RenderGraph/Frame/FrameResources.h`) keeps one texture per name for the
  whole frame. The same shape returns the same texture, whoever registers. A history or persistent texture is a versioned slot: a new
  size is its next version, so no graph is built again and every stage follows it; the passes declare the slot. Each stage's graph
  imports the texture; no graph makes it. A history's first clear is added only when the texture is new. The image-space port's targets
  (`MakeOwned`, `MakeOwnedLike`) come from it; a future port's cross-stage targets should too.
- At a staged build's switch-in the replaced build's recordings go, with its graph and pools.
- Only targets kept across frames are registered by name. A registered resource got a backing in every graph holding the client.
- ORG: a graph's end gives back the alias placements it made that no later graph or standalone backing replaced
  (`ReleaseOwnAliasPlacements`).

Measured: the resources listed here 1,642 to 1,471 MiB (g1n to g5n); process dedicated VRAM native 3,254 MiB (plain perf-taa run, against
3,395 to 3,514 before this pass), guest 3,036.

Left: `ImageSpace::GetHitA/B/Swap` and `FullBlurDown/Swap` (24.7 MiB) are still backed with no pass in any live graph. The give-back
runs for them (logged, 3 given back at the TAA switch build's end), but something gives them a standalone backing again afterwards; not
found. One transient object placed by two live graphs would still be placed twice; no such case is left in this scene.

### The survey

Verdicts: **keep** (needed as is), **remove** (nothing in native reads it), **transient** (written and read within a known span of the
frame: aliasable by the next agent), **on-demand** (only a rare path uses it), **one copy** (a duplicate). Stages: start, pre-depth,
pre-main, deferred+water (dw), sun, post, UI, menu UI; DCLF = Drawcall Limit Fix's graphs. Sizes are after this pass (g5n), MiB.

| Resource | MiB | Owner | Written / read by | Survives the frame | Verdict |
|---|---|---|---|---|---|
| `Engine::Depth::kSHADOWMAPS`, `_ESRAM` | 264 + 66 | engine target | DCLF shadow views; pre-depth, pre-main, post, menu UI, DCLF | yes (cached cascades) | keep |
| `Skylighting::ProbeArray`, `ShadowBitmask`, `AccumFramesArray`, `ShadowVisibility`, `Occlusion` | 112.5 | CS feature | pre-main; dw, DCLF | yes (accumulation) | keep |
| `TerrainShadows::ShadowHeight`, `HeightMap` | 49.2 | CS feature | pre-depth (a stripe a frame); dw, post, DCLF | yes | keep |
| main depth, `kMAIN_COPY` depth, `TerrainBlending::BlendedDepth16`, `kREFRACTION_NORMALS`, `kUNDERWATER_MASK`, `kMAIN_COPY`, the water displacement pair | 41.6 | engine targets | see "Engine targets still used"; read by the next frame | yes | keep |
| `kMAIN`, `Upscaling::UpscaledTexture`, `HDR::UiTexture` | 23.7 | engine / CS | every world stage; post; the UI stages | `kMAIN` read by Scaleform and menus | keep (Upscaled: Streamline submissions ORG does not track) |
| SSGI history (`IlY`, `GiSpecular`, `IlCoCg`, `PrevGeo`, `AO`, `AccumFrames`) | 59.4 | CS feature (port) | dw | yes (ping-pong) | keep |
| `ImageSpace::SAO`, `SAORawAO0/1`, `TAAHistory0/1`, `Bloom`, `AdaptedLuminance0/1` | 40.1 | frame (`Frame::Resources`) | dw (SAO), post (TAA, bloom, adaptation); DCLF reads the adaptation | yes | keep (one copy now) |
| normals targets (`kNORMAL_TAAMASK_SSRMASK`, `_SWAP`) | 15.8 | engine targets | pre-main, dw; post, menu UI (by the engine's frame counter) | one of the pair, yes | keep |
| VL pair `kIMAGESPACE_VOLUMETRIC_LIGHTING`, `_PREVIOUS` | 7.9 | engine targets | post | yes (by parity) | keep |
| Dynamic Cubemaps, Cloud Shadows layers, LLF lists, Volumetric Shadows copy, Grass Collision, `kREFLECTIONS` cube, other CS feature state | ~70 | CS features | start to dw, DCLF | yes | keep |
| unnamed native textures (`Native::Texture2D` x9: 1920x1080 R32 with mips, 2048x2048, 720x1071, ...) | 24.7 | made through the native device | not in any graph | ? | keep (owners not identified) |
| `kBOOK_TEXT_0..3`, `kBOOK_TEXT` depth | 45.0 | engine targets | declared by every Scaleform pass of UI and menu UI; drawn only in the book menu | no | on-demand |
| `kWATER_1/2` | 31.6 | engine targets | cleared by the frame clear group and dw; the water TAA form (`NoPass`) would draw them | yes (water TAA) | on-demand |
| `HDR::HdrTexture`, `HDR::OutputTexture` | 23.7 | CS feature | HDR output only (off here); made at setup so the switch needs no setup | no | on-demand |
| `ImageSpace::GetHitA/B/Swap`, `FullBlurDown/Swap` | 24.7 | frame (transients) | post's get-hit and full-screen blur steps, absent by key here | no | on-demand: GetHit given back and freed now ("Unbacked engine targets"); FullBlur's 1 MiB stays (a standalone transient no graph end gives back) |
| `kMENUBG` | 7.9 | engine target | post's capture copy (capture frames); menu UI's frozen composite | while a menu freezes the world | on-demand |
| `kSCREENSHOT`, `kLOCAL_MAP`, `kLOCAL_MAP_SWAP`, lens-flare targets | 13.4 | engine targets | console screenshot (`NoPass`), the local map (map menu), lens flares (`NoPass`) | no | on-demand |
| `Bink::Movie` | 9.0 | CS (Bink port) | UI and menu UI, movie frames only | no | on-demand (kept for the session so a movie never rebuilds the Present graph) |
| engine `kSAO`, `kSAO_RAWAO`, `kSAO_RAWAO_PREVIOUS`, `kSAO_TEMP_BLUR`, `kTEMPORAL_AA_ACCUMULATION_1/2` | 47.5 | engine targets | none: the port draws into its own | - | unbacked (2026-11-17): the port's image is the one; the engine's TAA history clears are declared dropped |
| engine `VOLUMETRIC_LIGHTING_ACCUMULATION`, `_COPY` (two of each: the engine makes them twice and both stay) | 45.0 | engine 3D targets | none: ORG's `VolumetricLighting::Volume` replaces them | - | unbacked (2026-11-17) |
| `kIMAGESPACE_TEMP_COPY/2`, `kTEMPORAL_AA_UI_ACCUMULATION_1/2` | 63.3 | engine targets | none (the post chain works in place; TAA's UI form is `NoPass`) | - | unbacked (2026-11-17); the Tonemap anchor's engine fallback names no unbacked target |
| `kGETHIT_BUFFER`, `kGETHIT_BLURSWAP`, `kLDR_BLURSWAP`, `kLDR_DOWNSAMPLE0` | 17.8 | engine targets | none: the port's get-hit and depth of field steps draw into CS transients | - | unbacked (2026-11-17) |
| `kNORMAL_TAAMASK_SSRMASK_DOWNSAMPLED` | 2.0 | engine target | the frame clear group clears it; SSR reads CS's `SSRMaskDownsampled` | - | unbacked (2026-11-17); its clear pass is gone and the engine's clear is declared dropped |
| `cs.native.texture.261` (1024x1024, 3 mips, R11G11B10) | 5.3 | Effects11's `TextureManager` shared downsample texture (made at its setup) | Effects11's D3D11 downsample, when an effect reads it | yes | keep (a CS feature's texture, not an engine target; identified 2026-11-17) |
| Deferred's G-buffer `cs.native.texture.270..275` | 43.5 | CS (Deferred, native) | pre-main (clear), DCLF opaque, dw (composite, last read); DCLF's Z-prepass, forward and first-person passes bind it | no | frame arena (2026-11-17): group placed for depth to first person; post's transients share its block |
| `cs.scaleform.scratch.0..3` | 31.6 | CS (Scaleform port) | UI and menu UI (GFx filters) | no | kept out (2026-11-17): every draw declares a read of them, and the role targets' draws come before the scratch draws, so the first use in the frame is a read |
| SSGI and SSS transients (`IlYBlurInput`, `Radiance`, `RadianceTemp`, `IlCoCgBlurInput`, `Normal`, `WorkingDepth`, `AccumFramesScratch`, `SSS::DiffuseNoAlbedo`) | 62.7 (one pool) | CS feature (port) | dw | no | frame arena (2026-11-17): dw's range, shared with post's and pre-depth's |
| SSGI `IlYScratch`, `GiSpecularScratch`, `IlCoCgScratch`, `AOScratch` (committed, not pooled) | 43.5 | CS feature (port) | dw | no (written and read in dw) | transient, kept committed (2026-11-17): exported to D3D11, and dw is the peak of their memory class, so the arena would save nothing |
| `ImageSpace::HdrScratch`, `LdrScratch`, `SAOBlurTemp`, `SSRMaskDownsampled`, `DofDownsample`, `DofBlurSwap`, `BloomSwap` | 28.6 | frame (transients) | post (scratch, depth of field, bloom); dw (SAO blur, SSR mask) | no | frame arena (2026-11-17): in post's and dw's ranges |
| `VolumetricLighting::Volume320x192x90` | 11.3 | CS feature (port) | post | no | frame arena (2026-11-17): post's range, inside dw's bytes |
| `kSHADOW_MASK`, `kMOTION_VECTOR`, `kTEMPORAL_AA_MASK`, `kMAIN_ONLY_ALPHA` | 23.7 | engine targets | pre-main or DCLF write; dw, post read (TAA, SSR, SAO) | no | transient: pre-main to post |
| `TerrainBlending::TerrainDepth`, `BlendedDepth`, `Mask`, `ScreenSpaceShadows::ShadowTexture` | 31.0 | CS features | pre-depth, pre-main; DCLF opaque, menu UI | no (BlendedDepth16 is the next frame's) | transient: pre-depth to opaque |
| `kPOST_ZPREPASS_COPY` depth | 11.1 | engine target | pre-main's copy; dw, post, sun, DCLF | no | transient: pre-main to post |
| `kRAW_WATER`, `kSNOW_SWAP`, `kSNOW_SPECALPHA`, `kSSR`, `kSSR_RAW`, `kSSR_BLURRED0` | 27.7 | engine targets | dw (water, snow, SSR) | no | transient: dw only |
| `kREFRACTION_NORMALS Copy`, `kUNDERWATER_MASK Copy`, `kMAIN Copy`, `kIMAGESPACE_VOLUMETRIC_LIGHTING_COPY` | 20.3 | engine copy textures | post (refraction, underwater), dw | no | transient: within one stage |
| `VolumetricShadows::ShadowBlurTemp` | 1.3 | CS feature | pre-depth | no | frame arena (2026-11-17): pre-depth's range |

Summary (g5n, the resources above): keep 119 resources, 783 MiB; transient 51, 352 MiB; on-demand 22, 155 MiB; remove 18, 181 MiB; one
copy none left. Before this pass (g1n): one copy 6 resources and 80 MiB of history, and 131 MiB more of transient copies.

### For the second agent (transient aliasing across stages)

Each CS stage's transients sit in its own private alias pool, and engine and feature targets that live only part of the frame are
committed images. Candidates, with the span each needs:
- Deferred's G-buffer (43.5): pre-main to dw. Never live with Scaleform's scratch (UI stage) or post's transients.
- Scaleform scratch (31.6): UI only. Can take the G-buffer's or the SSGI pool's memory.
- SSGI and SSS pool (62.7) and SSGI's committed scratch halves (43.5, if they hold nothing across frames): dw only.
- Post's transients (`Hdr/LdrScratch`, depth of field, bloom swap, the VL volume: about 30): post only.
- Engine targets with a known span: dw-only water, snow and SSR targets (27.7), the copy textures (20.3), `kSHADOW_MASK`,
  `kMOTION_VECTOR`, `kTEMPORAL_AA_MASK`, `kMAIN_ONLY_ALPHA` (pre-main to post, 23.7), Terrain Blending and Screen Space Shadows (pre-depth
  to opaque, 31.0), `kPOST_ZPREPASS_COPY` (pre-main to post, 11.1). These are engine targets or exported twins: aliasing them needs the
  recipe's span as their lifetime, not one graph's.
- One arena for every stage: each CS stage's transients are kept out of the shared arena (`SetPrivateAliasEpochs`) because another
  epoch's transients took their memory between two runs. A shared arena placed by the recipe's spans would avoid that.

### Not done in this pass (items 3 and 4)

Engine targets nothing in native uses (the "remove" rows, 181 MiB) still have memory. The clean form needs:
- BasicRHI: images whose memory is bound later or never, with views made now (Vulkan sparse binding, D3D12 reserved resources), or
  native device records without an image (`NativeResource::Kind::Shell` is the model);
- the native device: such a record is a fatal recipe mismatch at its first GPU use (a draw, clear or copy that reaches it), not at a bind
  the engine makes before a withheld draw;
- EngineTargets: the recipe's set of targets any stage or reachable old path can touch, and the others made unbacked in native.
Rare-path targets (book, local map, screenshot, lens flares) would take memory while a build that uses them is live; the book and map
menus and a screenshot cannot be reached by a background run, so those stay backed until a test reaches them. Item 4 (one image where the
port draws into its own copy: SAO, TAA, VL accumulation) follows from item 3: the engine's are unbacked, the port's stay.

### Checks (build `46485926fbab`)

One background run each: native-quick (105.7), native-river (river-east mean difference 3.5 against the earlier run), native-interior
(99.3 / 94.9, within the known variance), native-validation (the known 20 `VUID-vkCmdDraw-None-09600`, back-buffer layout lines and the
known unused-output warnings only), the loading-thread run, the worldspace run (Tamriel, Solstheim, Riverwood: shadow height map versions,
passed), guest-quick (106.1), and a frozen-world run with TAA on (`Journal Menu=show`, shot, hide, shots 5 and 65 frames after): the menu
opened (a frozen-world capture frame and menu UI frames), and the world before and after reads 104.1, 103.8, 104.4: TAA, SAO and the
adaptation carry across the menu. Pre-existing, not from this pass: one `[C] PublishedResources used on thread ..., not the render owner`
line in every native run, the state before this pass included.

## Unbacked engine targets (2026-11-17)

The "remove" rows of the survey: engine targets nothing in native mode uses, and the copies where the image-space port draws into its
own image. RX 6600, perf-taa at 1920x1080, background runs with `DXVK_DEBUG=hang`, VRAM Budget off. CS build `4487cdf39372`.

### The mechanism

- **The table.** `src/RenderGraph/Frame/EngineTargetUse.cpp` lists each engine target native mode keeps without memory, with what
  replaced its use (usually the port's own image). Every target not listed is backed. Guest mode backs every target (DXVK owns them).
  The goal stays one table of every engine target with its users; for now the backed ones are described in "Engine targets still used".
- **A record without an image.** `Engine::Targets` makes a listed target through `Native::MakeUnbackedTexture2D` / `3D`: a native
  texture handle whose record (`NativeResource::Kind::Unbacked`) has no BasicRHI image and no memory. The engine keeps the pointer, reads
  the description, and makes views on it as on any native texture. No sparse or reserved resources: nothing needs a bindable handle.
- **Any GPU use is a recipe mismatch** (`TapError::Unbacked`, through `Frame::Driver::NoteUnbackedUse`): a native context clear, copy,
  map, update or mip build on the record, an importer's `DescribeResource`, or a graph reader asking `Engine::Targets` for the target's
  slot (`Serve`). Fatal in development builds. In release (`CS_ORG_MISMATCH_FATAL=0`) the operation is skipped, the importer gets no
  texture, and each target's first use is logged; nothing touches a null image.
- **Declared-dropped clears.** The engine still clears some of these targets: `kNORMAL_TAAMASK_SSRMASK_DOWNSAMPLED` at the frame clear,
  and the four TAA histories when TAA resets. Their table lines say so (`dropClears`), and the native context drops those clears. The
  skeleton's frame clear group no longer has a pass for `kNORMAL_TAAMASK_SSRMASK_DOWNSAMPLED`.
- **Shape queries.** The image-space port's `MakeOwned` took the format from a graph import of the engine target; it reads the engine's
  description now. The Tonemap anchor's engine fallback (the frames before the chain registers, a menu scene's tonemap) names no unbacked
  target: the first run with the table stopped there, on `kIMAGESPACE_TEMP_COPY` at frame 598.

### What has no memory now

| Targets | MiB (g5n) | Why |
|---|---|---|
| `kSAO`, `kSAO_RAWAO`, `kSAO_RAWAO_PREVIOUS`, `kSAO_TEMP_BLUR` | 31.6 | the port draws into `ImageSpace::SAO*`, `SAOBlurTemp` (one image now) |
| `kTEMPORAL_AA_ACCUMULATION_1/2` | 15.8 | TAA's history is `ImageSpace::TAAHistory0/1` (one image now); the engine's reset clears are dropped |
| `kTEMPORAL_AA_UI_ACCUMULATION_1/2` | 31.6 | TAA's UI form has no CS pass; reset clears dropped |
| `kIMAGESPACE_TEMP_COPY/2` | 31.6 | the post chain works in place, with `Hdr/LdrScratch` |
| `kGETHIT_BUFFER`, `kGETHIT_BLURSWAP`, `kLDR_BLURSWAP`, `kLDR_DOWNSAMPLE0` | 17.8 | the get-hit and depth of field steps draw into CS transients |
| `kNORMAL_TAAMASK_SSRMASK_DOWNSAMPLED` | 2.0 | only the engine's clear wrote it (dropped); SSR reads `ImageSpace::SSRMaskDownsampled` |
| 3D `VOLUMETRIC_LIGHTING_ACCUMULATION`, `_COPY` (two of each) | 45.0 | the volumetric lighting port draws `VolumetricLighting::Volume` |
| `ImageSpace::GetHitA/B/Swap` (the defect below) | 23.7 | given back, and now freed |

No still-reachable engine path reads the engine's copy of SAO, TAA or the volumetric lighting volume, so no read had to be routed to
the port's image (a read would have stopped the runs).

### The GetHit re-backing (fixed in ORG)

The get-hit textures were not backed again: the give-back at the TAA switch build's end ran (`Dematerialize`), and nothing took a new
backing (ORG's new trace, `ORG_TRACE_STANDALONE=<name part>`, logs every backing a texture takes or gives back). The memory stayed because
`BackedResource` caches its last publication snapshot (`CapturePublishedBackingAllocation`), which holds a lease on the allocation. A
resource no graph uses is never published again, so the cache kept the old memory for the session. A dematerialized resource now drops
the cache (`ForgetPublishedAllocation`); publications already captured keep their own reference.

Left: `FullBlurDown/Swap` (1.0 MiB). The first post build backs them standalone (their score is below the alias threshold), and a
graph's end gives back only its alias placements, not its standalone backings. Giving those back needs an owner per standalone backing
across graphs (a later build may use the same backing); not done for 1 MiB.

### Measured

- The survey's resources: 200.2 MiB (g5n) to 1.0 MiB (dump of run u5n). Every live resource in the dump: 2,909 to 2,701 MiB.
- Process dedicated VRAM, perf-taa (plain runs): native 3,020 / 3,024 MiB, guest 3,033 (before: native 3,254 to 3,261, guest 3,035 to
  3,077). Native is at guest's level now.
- Frame period between the marks: native 19.11 / 19.04 ms, guest 19.09 ms.

### On-demand targets (not changed)

The build-key mechanism for targets (backing while a live or staged build has a pass that uses it) is not built. The targets stay
backed:
- waiting on a foreground session (their paths need input a background run cannot give): `kBOOK_TEXT_0..3` and its depth (book
  menu), `kLOCAL_MAP`, `kLOCAL_MAP_SWAP` (map menu), `kSCREENSHOT` (console screenshot), the get-hit blur steps;
- reachable by background runs, next for this mechanism: `kMENUBG` (the frozen-world menu run), `kWATER_1/2` (the water blend steps);
- lens-flare targets: their composite forms are `NoPass`, so they could be unbacked outright once a run with lens flares confirms no
  other reader.
`cs.native.texture.261` is Effects11's `TextureManager` downsample texture (R11G11B10, not RGBA16F): a CS feature's, kept.

### Checks (build `4487cdf39372`)

One background run each, all passed: native-quick, native-river (river-east mean difference 3.3 against the previous pass's run),
native-interior (100.6 / 97.2, within the known variance), native-validation (the same messages as the previous pass's run), the
loading-thread run, the frozen-world menu run with TAA (world before and after the Journal Menu 104.1, 103.7, 104.3, as before), the
worldspace run (Tamriel, Solstheim, Riverwood), guest-quick. No `[C]` line but the known `PublishedResources` one (in guest runs too).
The native D3D12 run was not taken (it loses the device on the RX 6600 for an unrelated reason).

## One arena for the frame's transients (2026-11-17)

Each CS stage's transients sat in a private alias pool, so the frame held the sum of the stages' peaks. Now one arena holds every CS
stage's transients and Deferred's G-buffer, placed by their span of the frame. RX 6600, perf-taa at 1920x1080, native Vulkan,
background runs with `DXVK_DEBUG=hang`, VRAM Budget off. CS DLL `5242f7b0ed1d`.

### The mechanism

- **ORG: `FrameAliasArena`** (`include/Render/RenderGraph/FrameAliasArena.h`). A frame position is a stage's place in the frame. A user
  reserves a byte range for a span of positions. Ranges whose spans overlap get disjoint bytes; the others share bytes. Ranges of one
  lane (an epoch's builds, which never run in the same frame) may share bytes whatever their spans. The search is a first fit
  (`alias::PlaceSpanned`, covered by the alias packing tests). Memory comes in blocks: a range goes in the first block of its memory
  class where it fits, else in a new block of its size. Reservations never move, so a block never grows. A block is freed with its
  last range.
- **A stage's transients.** `GraphHost::SetFrameAliasSpans` gives each CS stage's epoch its position. Its graph's planner packs each
  pool as before (the same packer, so aliasing inside the stage stays) and reserves one range of the arena for the packed size, with
  the stage's span and lane. Each plan reserves again and gives back the previous ranges. A graph's end gives back its last ones. The
  memory classes stay apart as the pools kept them (render targets and depth, other textures, buffers).
- **Groups.** Resources several stages use are declared in `src/RenderGraph/Frame/Transients.cpp` with their span, and placed once at
  a fixed offset (`FrameAliasArena::ReserveTextures`). The only group is Deferred's G-buffer: depth to first person, the stages whose
  graphs bind it. DCLF's Z-prepass, forward and first-person passes declare it as a render target too, so the span covers them. The
  native device makes a group's textures without memory inside a `Native::FrameTransientScope` (Deferred's setup) and places them
  together when the scope ends. Until then a record has no image, so a GPU use is `TapError::Unbacked`.
- **Positions** are the world recipe's stages in order (`Recipes::StageId`), then the menu UI stage. A stage's transients span its own
  position. A group's span is declared, and the recipe check holds the passes to it.
- `CS_ORG_FRAME_ARENA=0` (cs-test `org.frame_arena`) restores the private pools and the committed G-buffer, for A/B runs. Guest mode
  runs no CS stage and is unchanged. The declarations are per stage position, so guest can adopt them when it moves onto the stages.

### Hand-over and non-overlap

- **The alias ledger orders every hand-over.** A placed resource whose bytes another occupant used since its own last use enters its
  next execution from Undefined, and its first batch waits for that occupant's last use on every queue (ORG's
  `AliasAccessAdmissionLedger`, the same mechanism the shared arena used between epochs). It works for any placed resource, a group's
  included, so the layouts and barriers at a hand-over come from the ledger, not from a table. Async compute is covered: the waits
  cross timelines.
- **The spans keep content apart.** Stages commit in recipe order on the render thread, so the ledger sees the frame's uses in recipe
  order. A group's bytes are shared only with ranges whose spans lie outside its span, so nothing commits into them between its first
  and last stage, and its content survives within its span. A stage's transients live within one execution of its graph, so they need
  nothing beyond the ledger's ordering.
- **The recipe check** (`Recipe.cpp`, `Validate`) fails a pass that uses a group's resource outside the group's span, and a group whose
  first use in the frame is a read (its bytes come in holding another resource's data). Fatal in development builds, logged once in
  release, as the recipe's other findings. The first build caught Scaleform's scratch images this way (below).
- Each frame's alias activations are the same while the frame kind is the same, so admissions keep their predicted plans. The frame
  period is unchanged (below).

### Rebuilds

A stage's build (a key change, a resize) plans in its own state: its new ranges are reserved while the old build's are still held.
Two builds of one epoch share a lane, so the new build may take the old build's bytes. The two never run in the same frame, and the
ledger orders the old build's last execution before the new one's first. No placement a live build uses ever moves: a new packing is
the new build's own ranges and takes effect when that build switches in. A group is placed when it is made and goes with its textures
(a resize makes it again). Nothing waits on the render thread. In the runs the deferred+water stage reserved 5 to 7 times during the
load (the TAA switch and key builds), then not again.

### What shares memory (perf-taa, from the arena's census line and the alias plans)

| Block (class) | MiB | Ranges |
|---|---|---|
| render targets | 43.6 | G-buffer 43.5 (depth to first person); post's range 7.9 (`Hdr/LdrScratch`; 31.6 while TAA was being set up) |
| render targets | 9.9 | deferred+water's range |
| other textures | 49.5 | deferred+water's range 49.5 (SSGI and SSS transients); post's range 11.2 (the VL volume); pre-depth's range 1.2 (Volumetric Shadows' blur) |
| total | 103.0 | against 123.3 with the arena off on the same build: the same pools as private pools, and the G-buffer committed |

Saved: 20.3 MiB of transient memory at 1080p: post's 7.9 MiB render-target range and 11.2 MiB volume, and pre-depth's 1.2 MiB, all
inside other stages' bytes. The live-allocation dump agrees (device-local 2,227.8 against 2,248.6 MiB, runs c1n and c2n). Process
dedicated VRAM moves a little less (about 17 MiB): BasicRHI's allocator sub-allocates from 64 MiB heaps, so space freed inside a heap is
not returned.

### What stayed out, and why

- **Scaleform's scratch images (31.6 MiB).** Every draw pass declares a read of every scratch image, and the UI stage's plan puts the
  role targets' draws (surfaces 0 to 3) before the scratch images' own draws in each instance. So the first use in the frame is a read.
  In the arena those bytes would hold another stage's data then, and a filter's composite would show it. Placing them needs the draws
  ordered by what each really reads. Found, not fixed: the same order means a GFx filter's composite reads its scratch image before
  this instance's scratch draw writes it, so a filter shows earlier content even without aliasing.
- **SSGI's committed scratch halves (43.5 MiB).** Written and read in deferred+water only, but exported to D3D11 (an external import
  with no placement the ledger sees), and deferred+water is the peak of their memory class: the arena would save nothing.
- **Engine targets.** Most of the survey's span candidates are reached by old native paths (EngineDraws, the frame clear group, the
  ImageSpace anchor, the menu UI stage's scenes), which run at places in the frame the recipe does not fix, so they stay backed and
  out of the arena: `kSHADOW_MASK`, `kMOTION_VECTOR`, `kTEMPORAL_AA_MASK`, `kMAIN_ONLY_ALPHA`, `kPOST_ZPREPASS_COPY`, `kRAW_WATER`, the
  SSR targets, `kMAIN Copy`. `kSNOW_SPECALPHA` has no writer in the world recipe. Terrain Blending's mask and Screen Space Shadows'
  texture are read by DCLF's depth stage before pre-main writes them, so they are not transient. `TerrainBlending::TerrainDepth`
  (pre-depth to pre-main) would need a block of its own: no saving. Left: the post-only copies (`kREFRACTION_NORMALS Copy`,
  `kUNDERWATER_MASK Copy`, `kIMAGESPACE_VOLUMETRIC_LIGHTING_COPY`, 12.4 MiB), which fit beside post's ranges. They are host-made images
  imported as `ExternalTextureResource`. Placing them needs ORG to give such an import the alias identity the ledger orders (heap,
  offset, size), and Engine::Targets to place its take-over set as one group. Not done.
- **On-demand backing (`kMENUBG`, `kWATER_1/2`).** Not built. `kWATER_1/2` are cleared by passes present in every build (the frame clear
  group and deferred+water), so a build-key rule alone would never unback them.
- **DCLF's graphs** keep the shared arena: their epochs run a varying number of times a frame, so their activations would change from
  frame to frame.

### Measured (perf-taa, RX 6600, uncapped, alternating)

| | Native, arena | Native, arena off | Guest |
|---|---|---|---|
| frame period between the marks | 19.12 / 19.06 ms | 19.08 ms | 19.12 / 19.08 ms |
| process dedicated VRAM | 2,996 / 2,997 / 2,999 MiB | 3,015 / 3,016 MiB | 3,023 to 3,038 MiB |

Before this pass: native 3,020 to 3,024 MiB, guest 3,033; periods 19.04 to 19.11 against 19.09 ms. No frame-time cost: the hand-over
barriers are entries from Undefined that the ledger made inside each stage's pool before, and no clear was added.

### Checks

One background run each, all passed (DLL `dced173c1f74`; the perf and A/B runs on `5242f7b0ed1d`, which only adds the switch to the test registry): native-quick, native-river (river-east mean difference 3.3 against the previous pass's run; the
river shot differs by an NPC and the wind), native-interior, native-validation (the same message classes and counts as before: 20
`VUID-vkCmdDraw-None-09600`, the back-buffer layout and unused-output lines), the loading-thread run, the frozen-world Journal Menu run
with TAA (the world before and after 104.5, 104.0, 104.5), the worldspace run (Tamriel, Solstheim, Riverwood), native-upscalers (DLSS,
FSR, TAA and XeSS, clean by eye against the 11-16 run), guest-quick. No `[C]` line but the known `PublishedResources` one. ORG's alias
packing tests pass.
