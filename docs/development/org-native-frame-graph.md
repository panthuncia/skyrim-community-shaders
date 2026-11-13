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
