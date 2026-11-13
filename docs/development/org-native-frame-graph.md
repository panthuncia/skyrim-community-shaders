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
| 2     | Native resources owned by the graph; exempt class for immutable textures and static buffers                   |                |
| 3     | The engine's immediate operations become graph work                                                            |                |
| 4     | One frame graph: hooks only capture; one prepare and submit at Present; BasicRenderer's present tail          |                |
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
