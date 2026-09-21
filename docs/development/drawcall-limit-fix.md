# Drawcall Limit Fix

Drawcall Limit Fix (DCLF) replaces the game's per-object opaque draw loop with GPU-driven indirect draws
that the render graph executes on DXVK's Vulkan device (see [OpenRenderGraph on DXVK](./render-graph.md)).
It is built in phases:

1.  **Scene capture** (done): CPU tables describing every object DCLF will draw.
2.  **Indirect pipeline** (current): Vulkan device-generated commands, shaders recompiled for the render graph,
    tables uploaded to the GPU.
3.  **Integration and parity**: the native loop skips what DCLF draws; the images must match.
4.  **GPU culling**: two-pass object occlusion culling at the Z-prepass.
5.  **Cutting back the native loop** and its CPU occlusion culling.

What the game binary does, as far as DCLF depends on it, is recorded in
[Skyrim engine notes](./skyrim-engine-notes.md).

## Phase 1: scene capture

Source: `src/Features/DrawcallLimitFix.{h,cpp}` and `src/Features/DrawcallLimitFix/`.

### Which objects

Static rigid geometry:
-   a `BSTriShape` (not dynamic, multi-index or LOD) with no skin;
-   a `BSLightingShaderProperty`;
-   a technique DCLF supports: None, Envmap, Glowmap or Parallax, including TruePBR materials;
-   no alpha blending, no decal flags, no LOD flags, no projected UV and no refraction;
-   not under a `NiSwitchNode` or `BSOrderedNode`, and not part of an actor.

Per frame it must also:
-   be visible, meaning not app-culled or hidden up to its tracked root;
-   be fully faded in.

Objects whose LOD fade inputs are undefined in the engine (see `LightingDescriptors.cpp`) stay native. So do
the few objects whose drawn technique cannot be known when the tables are built (`alpha-test-state`, see
the engine notes' batch renderer open question). Anything else stays on the native loop, which draws it as
before.

The reasons are counted per frame (`CS_DCLF_STATS=1`, or the feature's settings page).

### Tracking

`SceneTracker` detours the `NiNode` child functions (`AttachChild`, `DetachChild*`, `SetAt*`). From
whatever thread attaches or detaches, it pushes events onto a lock-free stack. Detach events carry the
geometry leaves collected on that thread while the subtree is intact.

`SceneStore` belongs to the render thread. Every Present (`Feature::Reset`, so also in menus) it:

1.  Refreshes the set of tracked roots. New roots are scanned and removed roots dropped. The roots are:
    -   the Static, Dynamic and MultiBound category nodes of each attached cell (the interior cell, or
        the loaded grid);
    -   the `BSMultiBoundNode` children of `TES::objRoot`, where the engine moves references into
        multibounds (exteriors) and rooms (interiors);
    -   the interior portal graph's rooms and shared node.
2.  Applies the queued events in order.
3.  Re-checks a slice of tracked geometry against its root, as a safety net for missed detaches.

### Tables

At the start of the main pass (`Feature::Prepass`, when `Deferred::StartDeferred` runs), `BuildFrame`
first walks the main-camera accumulator's batches. That yields, for every geometry, the pass the main pass
will draw this frame and the technique it is registered under. It then rebuilds the tables from the
tracked geometry the accumulator holds; the rest of the tracked set was culled by the engine and cannot be
drawn this frame. Phase 5 takes objects out of the accumulator, so it will need the whole set again.

| Table | Content |
| --- | --- |
| `objects` (`ObjectRecord`, 128 bytes, GPU layout) | world and previous world 3×4, world bounding sphere, geometry / material / pipeline indices, flags (alpha test, two-sided, alpha threshold, external emittance suppression) |
| `geometries` (`GeometryRecord`) | the game's vertex and index buffer, vertex description and stride, index count |
| `pipelines` (`PipelineKey`) | final vertex and pixel shader descriptors (as `State::ModifyShaderLookup` produces them for the deferred pass), two-sided, and the raw pass descriptor |
| `geometryConstants` (per pipeline) | the per-frame `PerGeometry` constants for that pass descriptor |
| `techniqueConstants` (per pipeline) | the `PerTechnique` constants (fog, colour output clamp, `VPOSOffset`), sampler filter modes, and the shadow mask binding |
| `permutations` (per pipeline) | Community Shaders' permutation buffer (b4) for the draw |
| `materials` (`MaterialRecord`) | the `PerMaterial` constants, textures, address and filter modes for each (material, pass descriptor) |
| `shading` (`ObjectShading`, 32 bytes) | the per-object `PerGeometry` pixel constants (LOD fades, alpha, emissive colour, SSR specular) |
| `lights` (`ObjectLights`) | Light Limit Fix's per-object `StrictLightData` (b3): room index and shadow mask channels |
| `draws` (`DrawSequence`, 48 bytes) | one indirect draw per object in the device-generated-commands token order, addresses filled in Phase 2 |

The pass descriptor is the accumulated technique: it is what `SetupTechnique` receives. Deriving it from
the property flags is not reliable while the native loop runs, for three reasons:
-   Some bits come from per-frame engine state: the light and shadow assignment, and DoAlphaTest, which
    depends on an early-Z global.
-   `GetRenderPasses` rebuilds a pass only when the light state changes, so the specular and envmap fade
    decisions a pass carries can be older than the current fade metric.
-   The fade values the draw reads are the property fields that same call stored.

`LightingDescriptors.cpp` still derives the descriptor from the flags, following `GetRenderPasses` and
`SetupTechnique` with the LOD fades and TruePBR's changes, because Phase 5 will need it. `CS_DCLF_STATS`
reports how often the derivation disagrees with the drawn technique outside the per-frame bits: in the
coverage runs, at most one object per frame, a specular fade crossing.

Material and geometry constants are not ported. `ConstantEvaluator` runs the engine's own
`BSLightingShader::SetupMaterial` and `SetupGeometry` outside the render loop, against stand-in shader
objects (engine notes: "Evaluating the Setup functions outside the render loop"). The results therefore
include every Community Shaders hook on those functions.
-   **Evaluation frequency:** materials once per (material, pass descriptor) per frame, geometry
    constants once per pipeline per frame.
-   **Per-object values:** World, PreviousWorld, the LOD fades, alpha, emissive colour and SSR specular,
    stored in `objects` and `shading`.
-   **The one port:** `SetupTechnique` binds real shaders, so it is ported (`EvaluateTechnique`) from the
    decompile.

Constant components the engine does not write keep a sentinel (`kUnwrittenBits`). Phase 2 uploads zero for
them. The engine leaves them undefined too; they belong to variables the permutation does not read.

What Community Shaders binds on top of the engine was checked draw by draw (see below):
-   **Per object:** the permutation buffer (b4), Light Limit Fix's `StrictLightData` (b3), and the
    alpha-test reference (b11).
-   **Per pass:** everything else Community Shaders binds for these permutations (b5, b6, b12, and every
    feature texture outside t0–t15). DCLF can bind it once per pass.
-   **Never read by these permutations:** the bone palettes (b9, b10) and Skin's buffers and textures
    (b7, t71, t74, t75).

### Checking it: capture parity

`CS_DCLF_CAPTURE_PARITY=1` compares every native main-pass lighting draw of an object in the tables with
the tables, and logs every 300 frames:

-   `location`: the player's cell.
-   `capture parity OK|MISMATCH` covers (bit for bit, except `EmitColor`, compared within 0.1% because
    time-of-day emittance moves between the table build and the draw):
    -   that the drawn pass is the one the accumulator walk found;
    -   the full pass descriptor, and the final shader descriptors;
    -   the world transform;
    -   `PerMaterial` constants, textures and address modes;
    -   `PerGeometry` constants (with the per-object values applied);
    -   `PerTechnique` constants (re-read at the draw because of the late `VPOSOffset` write), filter
        modes and the shadow mask binding;
    -   tracking misses: eligible geometry under a tracked root that is not tracked.

    Native constants are captured from the Map/Unmap detours, and only components the engine writes are
    compared.
-   `draw parity`: the `DrawIndexed` arguments and the bound index and vertex buffers.
-   `light data parity`: Light Limit Fix's `StrictLightData` (no strict lights in the main pass, room
    index, shadow mask channels).
-   `permutation parity`: the permutation buffer (b4), limited to the extra bits the Lighting shader reads.
    `IsSun` and `GrassSphereNormal` stay set from other shaders' draws.
-   `feature binding parity`: within a frame, every constant buffer b3+ and shader resource outside t0–t15
    must stay the same, and must not be rewritten, across the eligible draws. The exceptions are the
    per-draw buffers listed above.
-   `render state … ->`: the cull, depth, stencil, blend and alpha-test state the native draw used, per
    property-derived key. A key marked `VARIES` means the state is not a function of the key.
-   Diagnostics:
    -   statically eligible geometry drawn outside the tracked roots, with the parent chains of a few;
    -   variables the native shaders have that DCLF leaves unwritten.

Results with `CS_DCLF_TEST_COMMANDS=600:coc WhiterunDragonsreach;1800:coc BleakFallsBarrow01;3000:coc Riverwood`:

| Location | Tracked geometry | Objects in the tables per frame | Checked draws per frame | Table CPU per frame | Mismatches |
| --- | --- | --- | --- | --- | --- |
| Whiterun (exterior) | 9,256 | 915 | about 910 | 1.05 ms | 0 |
| Dragonsreach (interior, rooms and portals) | 2,690 | 921 | about 920 | 0.69 ms | 0 |
| Bleak Falls Temple (interior) | 6,442 | about 300 | about 300 | 0.30 ms | 0 |
| Riverwood (exterior, after the cell transition) | 10,078 | 200 to 1,200 (camera moving) | 540 to 1,200 | 0.63 to 1.23 ms | 0 |

Every other check (draw, light data, permutation, feature bindings) is OK in all four.

Community Shaders fixes that came out of it:
-   TruePBR wrote partly initialized constant arrays (`TruePBR.cpp`, now value-initialized).
-   Light Limit Fix's room index and shadow mask derivation moved into `LightLimitFix::GetRoomIndex` /
    `GetShadowBitMask`, shared with DCLF.

## Phase 2: indirect pipeline (done)

DCLF's objects are drawn every frame by the render graph with one indirect command stream per pass, into
off-screen copies of the main pass's targets; the frame itself is unchanged. `CS_DCLF_DEBUG_VIEW=1` copies
those targets over the native ones before the deferred composite, so the frame shows only what DCLF drew.

### Game resources

-   **Buffers and images.** The DXVK fork exports `dxvkGetInteropResourceInfo` (ordinal 135). Given a D3D11
    buffer, texture or shader resource view, it returns the Vulkan handle, offset, size and device address
    (images: create info, view, swizzle and the layout DXVK keeps them in) and marks the resource stable,
    the same lock DXVK's NVX interop uses. Buffers the game can map are rejected. `GpuResources` resolves
    each geometry's vertex and index buffers once and holds a reference while they are cached (Whiterun:
    806 buffers, 0 rejected).
-   **Textures** (`GpuTextures`). DXVK keeps every image it hands out in `VK_IMAGE_LAYOUT_GENERAL` and uses
    it between the graph's commands, which is D3D12's simultaneous-access contract. Each shader resource
    view is imported into BasicRHI without ownership, with simultaneous access, and gets a view in ORG's
    shader-visible heap (a slot from ORG's descriptor service, retired against ORG's fences on eviction).
    A null view (the engine binds none) reads zero through a null descriptor, as D3D11's null SRV does.
-   **Samplers** are copies of the renderer's own D3D11 sampler states, the table the engine selects from by
    the shadow state's address and filter modes (engine notes: samplers).
-   **Per-frame constant buffers and dynamic structured buffers** are never locked: `ConstantMirror` keeps
    a CPU copy of each one DCLF watches, fed by the immediate context's `Map`/`Unmap` and
    `UpdateSubresource`, and re-reads mapped buffers from their last mapping (the engine writes some
    constants after `Unmap`). CPU-written structured buffers the pass reads (t98) are copied into graph
    buffers each epoch. Light Limit Fix's lights, light list and light grid (t35-t37) are its graph buffers.

### Pipelines and bindings

Every draw pushes one address, of its binding record (`DrawBindings`, 800 bytes: vertex and pixel constant
buffer addresses for b0-b13, resource heap indices for t0-t127, sampler heap indices for s0-s15). The
pipeline layout maps the shaders' registers onto that record with `VK_EXT_descriptor_heap`'s indirect
mappings; vertices come through the `VertexBuffer` argument. Lighting.hlsl needs no binding changes.

-   **SPIR-V.** `ShaderPrograms` compiles each pipeline key's Lighting VS and PS at run time through
    ORGModuleServices, with the defines of the D3D11 build (`ShaderCache::GetCompileDefines`), HLSL 2018,
    and register shifts per class (`ShaderPrograms.h`). DXC scopes cbuffers declared inside namespaces
    differently from FXC; those members are declared through `Common/NamespacedCBuffer.hlsli` (FXC output
    byte-identical).
-   **Pipelines** (`DrawPipelines`) are built asynchronously through ORGModuleServices' `PipelineService`,
    two per key: the main pass's (color, depth test EQUAL, no depth writes) and DCLF's own Z-prepass (depth
    only, LESS, writes). Vertex input is the engine's input layout for (VS input mask & geometry
    `VertexDesc`), ported from its builder (`VertexInput.cpp`); the pipeline key includes the geometry's
    vertex layout. A pipeline whose shaders read a register the layout cannot map, or a vertex input the
    geometry lacks, fails and its objects stay native. Both variants of a key get the same index in their
    indirect pipeline sets.
-   **Constants** are packed with the native shaders' constant tables (`LightingConstants`, shared with the
    capture parity check): PerTechnique per pipeline, PerMaterial per (material, pipeline), PerGeometry per
    object, Light Limit Fix's StrictLightData per (room, shadow mask), the permutation per (pipeline, object
    flags), the alpha-test reference per threshold, Linear Lighting's `LLPerGeometry` (PS b8, per object;
    Phase 1 missed that it is per draw) per multiplier, and every other bound buffer from its mirror.

### An epoch

At the first lighting draw of the main pass DCLF records what the pass binds; just before the deferred
composite (`Deferred::EndDeferred`) it runs the graph's `MainOpaque` segment. Its callback assembles and
uploads the constants, records, draw inputs and geometry table (about 2.5 ms of render-thread CPU and
1.6 MB for 915 objects in Whiterun). Then:

1.  `BuildDrawsCS` (compute) writes one `DrawSequence` per draw from the inputs and appends it with an
    atomic count, the shape Phase 4's culling will extend.
2.  The main-opaque pass executes the sequences twice with the GPU count: the depth variant into DCLF's own
    D24S8 depth, then the color variant (EQUAL) into eight graph-owned targets with the main pass's formats.
    The render area, viewport and depth range are the main pass's (the engine draws with depth range
    [0, 0.999998]; BasicRHI's `PassBeginInfo` gained the range).

The graph holds every feature's passes and each epoch executes all of them; passes record only in their own
segment (`RenderGraphRuntime::Segment`: LightCulling, MainOpaque, DebugView) and include it in their
invocation revision. An empty pass is effectively free.

### Gate

-   `CS_DCLF_BUILD_PARITY=1`: BuildDraws' output (read back through D3D11 wrappers of the graph buffers)
    equals the CPU templates. OK on every check in Whiterun (915 draws), Dragonsreach (921) and Bleak Falls
    Barrow (about 300).
-   The debug view shows the drawn objects textured, lit and depth-tested; nothing is skipped.
-   No validation errors besides the upstream `vkQueueSubmit2-semaphore-03868`.
-   One ExecuteIndirect per pass (two passes: depth, color) by construction; not yet confirmed in a capture.

### Fixed in shared components on the way

-   **ORG** `PersistentGraphHost` never released upload pages or retired descriptors: every page leaked
    (about 9 GB after a minute of DCLF uploads). It now keeps a per-slot frame timeline, waits for a slot's
    previous frame before reusing it, and calls the upload and descriptor deferred releases.
-   **ORG** persistent execution ignored `commonLayoutOnly` on external textures and transitioned DXVK's
    depth buffer out of GENERAL; `CompileResourceShape` now carries it and compiled states keep layout
    Common. `ExternalTextureResource` also reports the view flags its description implies (persistent graphs
    rejected its depth-stencil views).
-   **BasicRHI**: per-stage descriptor-heap mappings; depth-stencil and packed 10/11-bit formats; images
    imported with simultaneous access (views, attachments and transfers in GENERAL); SRV component mappings
    on Vulkan; null SRVs on both backends (Vulkan: `robustness2` `nullDescriptor`, which the DXVK fork now
    reports); the viewport depth range in `PassBeginInfo`.

### The hybrid path (Phase 3, in progress)

`CS_DCLF_HYBRID=1` makes DCLF draw into the main pass's own targets and depth, and the native loop leave
those objects to it (`DrawcallLimitFix::SkipNativePass`, on the `RenderPassImmediately` call sites Light
Limit Fix hooks, inside the main camera's depth and opaque ranges only). The decision is one frame old,
because the epoch runs after the native passes.

Depth is written in its own segment (`RenderGraphRuntime::Segment::ZPrepass`), at the **end of the native
depth pass**: everything the rest of the frame derives from the depth buffer - the native draws' own EQUAL
test, the sky, Terrain Blending's blended depth and every effect that reads it - has to see DCLF's objects.
The colour pass then runs before the composite and tests EQUAL against it, as the native main pass does.

The two epochs assemble separately, from their own captures, because the depth pass has not bound the main
pass's pixel-stage state yet. They draw the same tables with the same camera, so their depths agree. Only
the colour epoch records what it drew (`drawnFrame`), so the native loop can never skip an object that the
Z-prepass drew but the colour pass then left out - that would leave a hole that writes depth and shows the
background.

Whiterun exterior: 942 objects drawn, 1884 native passes skipped, 2.7 ms of render-thread CPU.

#### The depth-only pipeline variant

Running the Z-prepass inside the native depth pass is only possible because its pixel stage reads nothing
that is not bound yet. Both pipeline variants used to share the full Lighting pixel shader, which reads the
main pass's per-frame constants and textures. The depth variant is now built from its own SPIR-V of
`Lighting.hlsl`, compiled with `DCLF_DEPTH_ONLY`, which returns immediately after the alpha test:

-   alpha testing is a compile-time permutation (`DO_ALPHA_TEST`), so the cut is exact - the alpha-tested
    permutations keep their discard, the rest compile down to nothing;
-   everything past it is dead code, so the compiler drops the per-frame pixel bindings with it;
-   `RegisterUsage` is now kept per variant, so a draw only has to supply what its own variant declares.

The assembly for that epoch therefore skips the pixel-stage per-frame constants, the frame textures at t16
and up, and the structured-buffer copies - all of which come from the main pass's capture - and keeps the
material textures, samplers and vertex-stage constants, which come from the tables.

#### The ghost background, and what depth the rest of the frame reads

Writing the depth at the first draw of the main pass fixed the native draws, the sky and the composite, but
the frame still showed terrain and LOD washed over DCLF's objects. The cause is that the two depth
resources the rest of the frame actually reads are built at the **end of the native depth pass**, before
DCLF has drawn anything:

-   the engine's prepass depth copy (`kPOST_ZPREPASS_COPY`), and
-   with Terrain Blending on, its blended depth - which it also points `kMAIN` and `kPOST_ZPREPASS_COPY`'s
    depth SRVs at for the whole rest of the frame (`TerrainBlending::Hooks::Main_RenderDepth`), so every
    effect reached through `Util::GetCurrentSceneDepthSRV` reads it.

Both therefore held a scene without DCLF's objects, and each depth-reading effect painted the background
over them.

`DrawcallLimitFix::RefreshDepthConsumers`, called straight after the Z-prepass epoch, rebuilds both from
the depth buffer as it now stands: it re-runs `TerrainBlending::BlendPrepassDepths` and re-copies `kMAIN`
into the prepass copy, saving and restoring the render targets around it because the main pass is mid-draw.
That clears the wash completely in Whiterun exterior.

With the Z-prepass now running inside the native depth pass, almost none of that rebuilding is needed:
Terrain Blending builds its blended depth further up the same call site, after DCLF's thunk returns, so it
already sees DCLF's objects. DCLF installs its `Main_RenderDepth` hook before Terrain Blending does, which
is what makes its thunk the inner one of the chain and puts the prepass ahead of the blend.
`RefreshDepthConsumers` is now only the engine's prepass depth copy, and only when Terrain Blending is off
to redirect it - that copy is taken inside the depth pass, before the prepass runs.

#### The Z-prepass must draw exactly what the colour pass draws

Splitting the depth and the colour into two epochs introduced a trap that took a long time to find,
because every symptom pointed somewhere else: DCLF's objects rendered flat and grey, as though unlit.

The cause is an invariant, not a bug in any one pass. The Z-prepass was assembled without the main pass's
pixel-stage bindings, so it could draw objects the colour epoch would later drop for a missing constant.
Such an object ends up with depth in the buffer and nothing to shade it:

-   DCLF's own colour pass skipped it, so it writes no G-buffer;
-   the native loop would have drawn it - it is not in `drawnFrame`, so it is not skipped - but the native
    main pass tests depth EQUAL, and the depth it now finds is the one DCLF computed rather than the one
    the native prepass wrote, so every one of its fragments is rejected too.

Two changes keep the two sets equal:

-   The Z-prepass only draws objects the native loop is actually leaving to DCLF, which is the set the
    colour epoch drew in the frame before - the same rule `SkipNativePass` uses. Anything else keeps its
    native depth, and the native draw that owns it still works.
-   The three per-frame pixel buffers the depth-only shader reads before its alpha test - b5 SharedData,
    b6 FeatureData and b12 the game's PerFrame - are supplied from Community Shaders and from the game's
    own cache rather than from the main pass's bindings or the constant mirror, so the prepass can satisfy
    them during `RenderDepth`. With those in place both epochs assemble the same objects: Whiterun
    exterior and Dragonsreach both report **915 and 921 drawn with nothing skipped**.

Both epochs also rasterise with the main pass's viewport depth range. The game's depth pass and its main
pass do not use the same one - `[0, 0.999968]` against `[0, 0.999998]` - and the range scales the value
written to the buffer, so the same vertex would land about 500 D24 units apart in the two passes.

**How it was found.** By readback, after a long detour of guessing. The instrument that worked copies one
texel of every target, and of the depth, into a buffer from *inside* the epoch, as graph passes ordered
against the draws, and reads that buffer back a few frames later (`CS_DCLF_GBUFFER_PROBE=<x>x<y>`). An
equivalent D3D11 readback issued around the epoch is **not** ordered against ORG's submissions and
silently reports that nothing ever changes; it produced several confident and wrong conclusions before a
control - sampling a pixel only DCLF draws, and seeing no change - exposed it.

### Open
-   The sampler table is read at its AE 1.6.1170 address; other runtimes need its Address Library ID.
-   Material textures are imported per shader resource view; views of one image share nothing yet.
-   Records and constants are rebuilt and uploaded every frame; Phase 4 moves to persistent tables with
    dirty-range uploads.

### Why the indirect draws did not match the native ones

Phase 2 left an open question: testing depth EQUAL against the native Z-prepass rejected nearly every
fragment, and the error grew as objects came nearer. The answer turned out to be neither the arithmetic of
the vertex shaders nor the camera: **the pipelines drew the wrong side of every triangle.**

BasicRHI renders with a y-flipped viewport (it negates the viewport height, as DXVK does for D3D11), which
mirrors the winding of a triangle in framebuffer space. Its `RasterState::frontCCW` was passed straight
through to `VK_FRONT_FACE_*`, so leaving it at its default made the clockwise faces front-facing in flipped
space - the opposite of what the engine's meshes, wound for D3D's "clockwise is front", require. Every
pipeline therefore culled the faces it should have drawn and drew the ones it should have culled.

That single mistake produced all of the symptoms:

-   A flat surface (a floor tile, a wall panel) has no second side, so it disappeared entirely.
-   A closed object still rendered, from its far side, so it looked plausible but sat one object-thickness
    too deep. That is what made the depth comparison look like a systematic scale: along a floor, DCLF's
    view depth was a constant 1.0141 times the native one.
-   Depth EQUAL against the native prepass therefore failed almost everywhere, which is why Phase 2 needed
    a Z-prepass of its own.

The gap was closed in BasicRHI rather than worked around here. `RasterState::frontCCW` is now defined in
clip space - false, the default, is D3D's "clockwise is front"; true is glTF's - and the Vulkan backend
inverts it when it fills `VkPipelineRasterizationStateCreateInfo`, so one pipeline description means the
same thing on both backends and no caller branches on the API. DCLF therefore leaves `frontCCW` at its
default, which is already the convention the engine's meshes are wound for.

**How it was measured.** A throwaway depth probe allocated DCLF's depth as a readable texture and compared
it with the native depth on the CPU, with `CS_DCLF_ONLY_ELIGIBLE=1` restricting the native frame to the
objects DCLF also draws, so the two held the same scene and could be compared pixel for pixel. In
Dragonsreach:

| | before | after |
| --- | --- | --- |
| pixels DCLF covers | 2 508 133 | 3 667 129 |
| pixels only the native frame covers | 1 156 228 | 0 |
| pixels equal to the bit | 517 (0.02%) | 1 284 595 (35%) |
| difference, 5th to 95th percentile | +34 to +194 470 D24 units | -3 to +3 D24 units |
| view depth ratio | 1.0073 to 1.0202 | 1.00000 |

The remaining few D24 units are the FXC/DXC difference described below, which is two orders of magnitude
smaller than the bug it was hiding behind.

### The residual: FXC against DXC

The native shaders are compiled by FXC and reach the GPU through DXVK's DXBC path, which marks positions
invariant and keeps `precise` multiply-adds fused; DCLF's are compiled by DXC, which emits NoContraction.
Lighting.hlsl's vertex shader composes `mul(ViewProj, world4x4)` per vertex, whose intermediate terms carry
the object's world position, so the orders differ in the last bits and the perspective divide amplifies the
difference as objects come nearer. It is worth a few D24 units, and no correctness gate should depend on
the two compilers agreeing exactly: DCLF owns the depth of the objects it draws, and the native loop skips
them.

### A gap this left in the checks

Every parity check of Phases 1 and 2 passed while the frame was visibly wrong, because each of them
compares what DCLF *would* submit with what the engine submits: the tables, the constants (276 300 draws,
every block compared), the draw arguments and bound buffers, and the generated sequences. None of them
looked at what the indirect draws actually produced. The depth comparison above is that missing check, and
it is the one that found this. It was a throwaway instrument and has been removed again; Phase 3's image
parity (`CS_DCLF_PARITY`) is its durable form, and `CS_DCLF_ONLY_ELIGIBLE` is what gives it a native frame
holding the same objects.

## Phase 4: what the culling is given to cull

The tables used to be built from Skyrim's main-camera accumulator, which is the engine's output *after* it
has run its own frustum, occlusion-plane and room/portal culling. That made GPU culling impossible to
evaluate: in Whiterun the accumulator held 915 objects out of 9256 tracked, and frustum culling rejected
0 of them. A rejection count of zero reads like a clean pass, but it only meant the engine had already
removed everything the test could have caught.

`SceneStore::BuildFrame` now classifies the whole tracked set, and the engine's decision is kept per object
as `kObjectNativeVisible` rather than thrown away. Two things follow.

**The culling has real work.** The same Whiterun frame offers 3057 candidates, of which frustum culling
rejects 932.

**The engine is the reference oracle.** Because every candidate carries what the engine decided,
BuildDrawsCS cross-tabulates the two for free:

| | GPU keeps | GPU rejects |
| --- | --- | --- |
| **engine kept** | drawn | **false negative — a defect** |
| **engine culled** | rescued (expected: the GPU culling is the more conservative of the two until the HZB lands) | agreement |

The false-negative count is the gate. It is reported every 300 frames whether or not `CS_DCLF_STATS` is on,
because an object the engine kept and the culling here dropped is missing from the frame.

### The gate: which candidates may be drawn

`CS_DCLF_CULL_INPUT` decides whether a candidate the engine culled may actually be drawn. It defaults to
`native`, which draws only what the engine kept, so the frame is exactly what it was before while the
counters still measure the whole set. Whiterun, `native`, with frustum culling on:

    culling: 915 draws written, 932 of 3057 tested were rejected (30.5%);
    against the engine: 1210 it culled were gated out, 1210 it culled were kept, 0 it kept were rejected

915 + 932 + 1210 = 3057, and no false negatives. The frame is unchanged and there are no validation errors.

Two hazards came with widening the input, both worth remembering:

-   **Anything that marks an object "drawn" must be gated by what will actually be drawn.** The hybrid skip
    and the Z-prepass both key off `drawnFrame`. Marking a candidate the gate drops would make the native
    loop skip a pass DCLF never drew, and — because the Z-prepass draws exactly what the colour epoch drew
    last frame — would write depth for an object nothing then shades. That is the grey-object failure of
    Phase 3 all over again. The gate is a per-object flag test and so is predictable on the CPU; frustum
    rejection is not, which is precisely what the false-negative counter is for.
-   **Widen the readback with the counters.** The draw-count buffer grew to eight words but its D3D11 view
    was still four, so the two new counters read past its end and came back zero — indistinguishable from a
    clean result. The first run duly reported zero false negatives and zero rescued, and only the arithmetic
    (rescued had to equal the 1210 gated out) gave it away.

### `CS_DCLF_CULL_INPUT=tracked` is not yet correct

Handing the decision to the GPU culling alone works mechanically: 2125 objects draw, nothing is skipped for
missing resources, no buffer overruns, no validation errors. The shading is wrong, in a way that saturates
the frame red — so this mode is a Phase 4/5 target, not a usable setting.

Two known reasons, both structural rather than incidental:

-   Objects outside the accumulator never went through the engine's per-frame light and shadow assignment,
    so their pass descriptors come from the property derivation alone, with its guesses for
    `kRuntimePassBits`, and their `shadowBitMask` is zero. Making that derivation authoritative is Phase 5's
    stated prerequisite.
-   The colour pass tests depth `EQUAL`, and the Z-prepass draws only what the colour epoch drew last frame.
    An object the engine culled has depth from neither pass, so DCLF does not own its depth in the way the
    main pass requires.

The CPU cost of the wider input is also real and unaddressed: building bindings for 3057 candidates instead
of 915 takes 8.4 ms per frame against 3.2 ms. The culling has to move ahead of the per-object binding build
before this set size is affordable.

### The HZB and occlusion culling

`CS_DCLF_CULL=occlusion` adds a hierarchical-depth test after the frustum test. The pyramid is built by
`HzbCS.hlsl` in a pass at the end of the ZPrepass segment, which is the end of the native depth pass, so it
describes the depth the frame actually has: the native occluders the engine drew, plus DCLF's own depth
draws.

Decisions worth keeping:

-   **It stores raw NDC depth, not linear view depth.** Skyrim's projection is monotonic in distance and
    the test only ever compares two depths for order, so linearising would add a step that can be got wrong
    without making any comparison more correct.
-   **Every level is the maximum of the level below**, so a texel holds the farthest surface under it, and
    a footprint that is partly empty sky keeps a large value and nothing under it is culled. Texels outside
    the rendered area read the far plane for the same reason.
-   **Mip 0 is half the next power of two** of the depth, so the chain is a clean sequence of halvings and a
    level can be chosen from a screen extent by `log2` alone.
-   **The whole chain is one pass** with a full memory barrier between dispatches. Levels of one texture are
    not separate resources to the graph, so a pass per level would declare the same resource as its own
    input and output.
-   **A margin on the comparison.** The object's nearest corner is in raw clip space while the HZB holds
    what was stored, which the viewport depth range scaled — and the native depth pass and DCLF do not use
    the same range (`[0, 0.999968]` against `[0, 0.999998]`). The gap is small but systematically in the
    direction that culls, and it bites hardest on flat objects lying against the surface behind them.

Dragonsreach, with the whole tracked set as input:

    culling: 825 draws written, 865 of 1962 tested were rejected (44.1%: 181 outside the frustum,
    684 occluded, 96 of those the engine had kept); against the engine: 0 it kept the frustum test rejected

One validation VUID (the upstream semaphore one), and the frame is pixel-identical to a frustum-only
control run of the same scene.

#### The engine is not an oracle for occlusion

The cross-tabulation against the engine is a real instrument for the **frustum** test, where the engine is
exact and the two must agree — it holds at zero disagreement in both Whiterun and Dragonsreach. It is
**not** one for occlusion, and reading it that way cost a detour here.

The first occlusion runs reported dozens of "false negatives": objects the engine kept that the HZB
rejected. Recording one rejection in full rather than guessing at the count settled it in a single run:

    HZB rejection sample: farthest 0.997283 against nearest 0.998389, uv (0.4537 0.1599)-(0.4573 0.1624),
    mip 3, engine kept it

Those depths are w ≈ 5434 and w ≈ 9066, so the object was some 3600 units behind its occluder. It was
genuinely hidden. The engine's occlusion is planes, boxes and room/portal visibility, all of which keep
plenty of geometry that is in fact invisible — which is the whole reason for testing against a depth
pyramid. An object the engine kept and the HZB rejected is the expected win, not a defect. The counters now
keep the two apart, and whether such a rejection was correct is a question about visibility that only a
depth test can answer, not one the engine's opinion can settle.

#### Getting there: two wrong HZBs first

Both failures looked like plausible culling results and neither was:

-   **The depth was imported twice**, once for the draws to attach and once for the build to sample. The
    graph then saw two resources it believed unrelated, ordered nothing between them and inserted no
    barrier. One import carrying both a depth-stencil and a shader-resource view fixes it.
-   **The UVs were never scaled for the power-of-two padding.** The HZB covers a larger, power-of-two area
    than the rendered image, so a texture coordinate in the image is not one in the HZB. Without the scale
    the culling sampled mostly padding, which is all far plane, and the HZB looked uniformly empty however
    correctly it had been built.

What found both was a counter, not a screenshot: the number of sampled footprints whose farthest depth came
back `~0` or `~1`. 918 of 927 all-far says "this pyramid is empty" in one run, and no amount of staring at
a frame says that. The same counters are still there, and so is the one recorded rejection.

#### Still open in Phase 4

-   **The false-negative detector** (`CS_DCLF_CULL_VALIDATE`) is not built. It is the real gate: draw every
    culled object against the final depth with `LESS_EQUAL`, no writes and early-fragment tests, and flag
    any object that would have produced a fragment. Until it exists, "no objects were wrongly culled" rests
    on frame comparisons, which is weaker than the plan asks for.
-   **The two-phase replay** is not built. Today occlusion culling decides once, in the colour epoch,
    against the HZB built earlier in the same frame; there is no phase-1 list tested against the previous
    frame's HZB and no second depth draw to rescue its rejects. The consequence is that an object that
    becomes visible is drawn a frame late rather than rescued within the frame.
-   **Rigorous image parity** needs a scene that is not clipped. The Dragonsreach comparison is
    pixel-identical by eye, but the frame after a `coc` teleport is so overexposed that per-pixel deltas
    there measure the clipping, not the geometry.

## Phase 4.5 Step 0 and Step 1: the derivation, and what the stand-in leaks

### Step 0: the derivation counter now says what it measures

The counter that reports the property-only derivation against the accumulated pass descriptor used to be
masked with `& ~kRuntimePassBits`, so `0 differ` covered the 25 property bits and silently excluded the
four runtime groups (shadow light count 6-8, ShadowDir 13, DefShadow 14, DoAlphaTest 20) - which are
exactly the bits `GetRenderPasses` produces, and exactly the ones a Phase 5 that stops calling it would
have to derive. It also counted every object in the tables, although an object outside the accumulator has
no pass to compare against: `descriptors.pass` *is* the derivation for those, so they compared the
derivation with itself and could never differ.

Both are fixed. The report is now two halves plus a per-bit breakdown, over the comparable objects only.
Whiterun exterior, 3057 tracked:

```
derivation (last frame): 915 objects compared, 0 would stay native;
  property bits: 0 differ (00000000);
  runtime bits: 602 differ (00006000);
  per bit (* = runtime): bit 13*=602, bit 14*=602
```

The result is better than expected, and it changes what Step 6 costs:

-   The property half is exact: **0 of 915** differ.
-   The shadow light count (bits 6-8) and DoAlphaTest (bit 20) are **already derived correctly** - they
    never appear in the breakdown.
-   The whole gap is ShadowDir and DefShadow, always together, on about two thirds of the objects.

So skipping `GetRenderPasses` is one binary decision away, not four. Withholding registration (Step 5)
remains the right first move regardless, because it does not depend on deriving anything.

### Step 1: the artifact was a shared per-pipeline table, not a leak

**A reproduction that is not subtle.** The artifact was first seen as slightly blown-out additive lighting
in a night exterior, which is a poor instrument: the difference is small, and the time of day and weather
drift between runs. `coc WhiterunBanneredMare` at hour 22 is the opposite - a lit interior where the
defect covers the entire frame in white and red and cannot be mistaken for anything else. Every result
below is from that scene against a `CS_DCLF=0` control taken the same way.

**Two things that made every earlier comparison worthless.** `CS_DCLF=0` was read by nothing: it appeared
in the startup summary line and in no other code, so the feature installed unconditionally and every run
previously labelled a "`CS_DCLF=0` baseline" had DCLF fully enabled. And `CS_DCLF_TEST_COMMANDS` was
driven off `SceneStore`'s frame number inside `Prepass`, which does not run when the feature is off, so a
control never executed the commands and never reached the same cell at the same hour. Both are fixed: the
switch is a real gate, and the command driver has its own counter driven from `Reset`, ahead of the
install check, skipping loading-screen frames so the two runs stay in step.

**The cause.** Several tables are per-*pipeline*, not per-object, and the object that first creates a
pipeline fixes their contents for every object that shares it. The important one is
`tables.geometryConstants`, which comes from `FindLightingPass(property)` of that first object and carries
**that object's scene light list**, from which the engine reads the sun and the per-frame lighting.

While the tables held only the accumulator's output this was harmless: every candidate was visible, in the
lighting situation being drawn. Widening them to the whole tracked set made it a defect. A candidate the
engine had culled - in another room, or unlit - would often create the pipeline and hand its lights to the
visible objects drawn on it. In an interior that is a whole room lit by the wrong light list.

The fix is ordering, in `SceneStore::BuildFrame`: objects the engine kept are classified first, the rest
afterwards. An object the engine kept is by definition in the lighting situation being drawn, so it is the
correct template, and objects that cannot be drawn should never displace it. It costs one extra pass over
a hash map.

**How it was found, and two wrong turns worth recording.**

`CS_DCLF_EVAL` (new) suppresses parts of the stand-in evaluation. The first table it produced looked
decisive and was not:

| Configuration | Result |
| --- | --- |
| `CS_DCLF_TABLES=accumulated` | clean |
| `CS_DCLF_TABLES=tracked` | whole frame blown out |
| `… CS_DCLF_EVAL=off` | clean |
| `… CS_DCLF_EVAL=geometry` (only `SetupGeometry`) | clean |
| `… CS_DCLF_EVAL=material` (only `SetupMaterial`) | blown out |

This was read as "the per-material stand-in leaks state into the engine", on the further argument that the
corruption covered actors, which are skinned and never in DCLF's coverage. **Both halves were wrong.**
Suppressing the material evaluation makes `EvaluateMaterial` fail, which makes its objects ineligible, so
DCLF draws almost nothing - the "clean" rows were clean because nothing was drawn, not because nothing
leaked. And the actors were not corrupted; they were being washed out by the over-bright surfaces around
them.

The control that settled it was one that had never been run: **wide tables and the full evaluation, with
`CS_DCLF_HYBRID` off** so DCLF still builds and evaluates everything but its output never reaches the
frame. That frame is clean, which rules out leaked engine state entirely and puts the fault in DCLF's own
draws. The draw counts said the rest: `tracked` and `accumulated` draw the same ~300 objects in that
scene, so the difference had to be in what the shared tables contained, not in what was drawn.

The lesson is that a switch which suppresses a computation also suppresses everything downstream of it,
so "suppress it and see" only localises a fault when the downstream effects are held constant. The
`HYBRID`-off control does hold them constant, and should have come first.

### The rest of Step 1: evaluations that were pure waste

A candidate the engine culled cannot be drawn while the draws are gated on the engine's own visibility
(`CS_DCLF_CULL_INPUT=native`). It is still kept in the tables so the GPU culling has it as a candidate and
can be measured, but everything downstream of being drawn was being computed for it anyway - the
material's stand-in evaluation and the pipeline's per-frame constants, which are the two most expensive
things in `BuildFrame`.

Skipping those for undrawable candidates, in Whiterun exterior at night:

| | Before | After |
| --- | --- | --- |
| Material evaluations per frame | 636 | **132** (1783 skipped) |
| Material evaluation cost | 0.39 ms | **0.237 ms** |

The parallel per-object arrays are all still appended for a skipped candidate, with its material and
pipeline indices left at zero - nothing reads them, because `kObjectNativeVisible` is unset and
`BuildDrawsCS` rejects it first. That is what the earlier attempt at this got wrong: skipping one of the
arrays shifts every later object's index.

**The cross-frame material cache is not worth building, and the measurement says so.**
`CS_DCLF_MATERIAL_CACHE=probe` keeps the previous frame's record per (material, pass descriptor) and
compares it with a fresh evaluation. It reports **131 unchanged against 1 changed** per frame, so a cache
would hit about 99% - the worry that `SetupMaterial` reads enough per-frame engine state to make caching
unsound turns out to be wrong in practice.

But it would now save only the remaining 0.237 ms, against classification at 2.450 ms and the indirect
epoch at roughly 3 ms twice over. It also cannot be made exactly safe: the one record that does change
each frame is evidence that something outside the material's own fields feeds the result, so any cache
needs either a validity token that provably covers it or bounded-staleness re-evaluation, and neither is
worth it at this size. The probe switch stays so the decision can be revisited if the balance changes.

### `CS_DCLF_EVAL=audit`, and making `RunStandIn` transparent

Before the control above, the leak hypothesis was tested properly rather than by eye.
`CS_DCLF_EVAL=audit` snapshots the pipeline state around every stand-in call - VS and PS constant buffers,
PS shader resources 0-127, VS shader resources, PS samplers, and the bytes of the `BSLightingShader`
object - and reports every slot that differs after the restore, once per distinct finding.

The first version audited only the first 8 of roughly 640 material evaluations a frame and reported
nothing, which says nothing at all when the leaking call could be any of the other 630. Audited across
every call, it still reported nothing on the material path: the stand-in genuinely leaks no pipeline
state, which is what finally broke the wrong hypothesis.

It did find one real leak, on the geometry path: a stand-in `SetupGeometry` left a constant buffer bound
at PS slot 7 that was not bound before, because a feature hook binds its own per-geometry buffer there and
`RunStandIn` only restored the one slot matching the level being evaluated. That assumption is now gone -
all 14 constant buffer slots are saved and restored on both stages, which removes the class of problem
rather than the instance. The audit now reports zero leaks over a full run.

Light Limit Fix's `BSLightingShader_SetupGeometry_After` was a second real defect found on the way. It
publishes `strictLightDataCB` conditionally, keyed on four cache variables recording what the buffer
already holds, and binds b3 through a once-per-frame latch. A stand-in call updated all of it, so the next
real draw whose lights matched the cache skipped its own upload and shaded with the stand-in's lights.
`ConstantEvaluator::Evaluating()` - which already existed, documented as "hooks on the shader functions
must ignore it", and was consulted by nothing - now guards it.

Neither of these was the artifact. Both are fixed, and `RunStandIn` is transparent by the audit's measure.

**Result.** `CS_DCLF_TABLES=tracked` now matches a `CS_DCLF=0` control in the Bannered Mare at hour 22
with DCLF drawing 953 of 1102 objects, and the audit reports no leaks. `accumulated` is no longer needed
as a fallback.

### Two crashes and a false trail

`SceneStore::ProcessEvents` walked the scene graph every Present, including while a load screen was up,
and a load rebuilds `TES::objRoot` and the cell 3D under it. That crashed twice in the same subsystem: in
`RefreshCategoryNodes`' `objRoot` walk on a child that read back as `0x0001000000020001`, and the day
before in `AddSubtree`. The walks are now skipped while a load screen is up, the queued events are drained
but discarded, and the first frame afterwards rebuilds the tracked set from scratch. `BuildFrame` also
returns empty tables during a load: the tracked entries hold `NiPointer`s to the objects but not to their
renderer data, so classifying them across a load reads freed `BSGraphics::TriShape` data.

A `VK_ERROR_DEVICE_LOST` then appeared on `coc` teleports and was chased through five builds on the
assumption that it was one of these changes. It was not: the tree that failed four runs in a row passed
six times afterwards while byte-identical, so the bisect proved nothing and the fault was a persistent GPU
context left by the earlier crash. Worth remembering before bisecting an intermittent fault again - the
control has to be re-run at the end, not only at the start.

## Step 2: the per-draw epoch cost

The indirect epoch assembles one `DrawBindings` record per draw and runs twice a frame, for the depth
pass and the colour pass. The plan assumed the 128-iteration texture loop was the expensive part. It was
not, and the first thing built here was the instrument that says so: `CS_DCLF_STATS` now breaks the epoch
down by part. Bannered Mare at hour 22, 959 draws:

| Part | Before | After | |
| --- | --- | --- | --- |
| Textures and samplers | 0.286 ms | 0.258 ms | resolved per (material, pipeline) |
| **Constant groups** | **1.613 ms** | **0.538 ms** | packed per pipeline, patched per object |
| Binding record | 0.089 ms | 0.073 ms | |
| Rest (uploads, the epoch itself) | 0.710 ms | 0.773 ms | untouched |
| **Total per epoch** | **2.74 ms** | **1.62 ms** | 2.85 → 1.91 µs per draw |

### Textures and samplers, resolved once per (material, pipeline)

Neither loop depends on anything per-object: they read the material's textures, the pipeline's technique
(for the shadow mask) and its register usage, plus the frame's shared textures. Running them per draw
meant 128 + 16 iterations and a descriptor heap lookup each for every draw, when about 150 distinct
(material, pipeline) pairs stand behind ~950 of them. They are now resolved on first use and copied
afterwards, exactly as the constant blocks already were.

This is the CPU half of the plan's step 2a. The other half - having the shader read
`ResourceDescriptorHeap[…]` so the indices need not travel in the record at all - is not done, and on this
measurement is worth less than it looked: the loops were 10% of the epoch, not the bulk of it.

### The PerGeometry group: pack per pipeline, patch per object

This was the real cost. `PackConstantGroup` walks every variable in the layout and every component of
each, checking a written-sentinel and copying four bytes at a time; it ran twice per object, over a group
that is identical for every object on a pipeline apart from **five variables** - the two world matrices
and three shading values (`MaterialData`, `EmitColor`, and the w of `SSRParams`).

The group is now packed once per pipeline into a template, and each object copies the template and
rewrites only those five, at byte offsets resolved once per pipeline (`GeometryPatchOffsetsOf` /
`PatchObjectGeometry` in `LightingConstants.cpp`). An unwritten component still packs as zero, which is
what the full pack's initial `memset` produced for it.

It also removes the `GeometryConstants` struct copy per object - two `ConstantBlock`s, about 2 KB - which
`ObjectGeometryConstants` made only to patch four fields in it. That function stays, because the capture
parity check uses it as the reference implementation.

**Validated by capture parity, not by eye.** With `CS_DCLF_HYBRID` off so the native loop still draws
everything and the check has something to compare against: **125,100 draws checked, 0 mismatched (0
material, 0 per-geometry, 0 technique)**. That is a much stronger statement than a screenshot, and it is
the right gate for a change that only ever alters constant *values*.

### A crash this introduced, and the shape of it

Skipping the material and pipeline entries for undrawable candidates left their `materialIndex` and
`pipelineIndex` meaningless, and the first version left them at zero. The first frame after a teleport has
tracked geometry but nothing accumulated yet, so *every* candidate is undrawable, the pipeline table is
empty, and `pipelineBlocks[0]` is out of bounds - an access violation on `coc`. Index zero is not a safe
placeholder when the table can be empty. There is now an explicit `kObjectNoBindings` flag, checked before
anything indexes the tables with those fields, and such objects are reported under their own skip reason
(`candidate-only`) rather than being conflated with pipelines that are not ready yet.

### What is left of step 2

The record is still 800 bytes and still uploaded per draw; `rest` (uploads and the epoch) is now the
largest single part at about 0.8 ms. Cutting it means the bindless work proper - the shader reading its
textures and per-object constants out of GPU-resident tables indexed by a single object index, so
`DrawBindings` and the `IndirectAddress`/`IndirectIndex` ranges disappear. That is a pipeline layout and
`Lighting.hlsl` change (a `DCLF_BINDLESS` permutation beside `DCLF_DEPTH_ONLY`), and it is not started.

## Step 3: both epochs now share a table generation

The Z-prepass epoch runs at the end of `Main_RenderDepth` and `BuildFrame` ran at `Prepass`, from
`StartDeferred`, which is after it. So the depth segment read the **previous** frame's tables while the
colour segment read the current ones, and the per-object visibility verdicts the depth segment writes
could not be applied by index in the colour segment at all - object index *i* meant a different object in
each. That is what blocked two-pass culling.

### The measurement that was wrong, and the one that settled it

An earlier probe counted the accumulated passes at each candidate hook point and found 0 before the depth
pass against 604 at `Prepass`, and this was written up as "`BuildFrame` cannot run any earlier". **That was
wrong**, and the Ghidra decompilation is what exposed it:

-   `Main::Draw` (`0x1406444b0`) queues `DrawWorld_BuildSceneLists` (`0x14064bc20`) on the
    `gJobList_SceneListAccumCulling` job list and **`JobList__Finish`es it before**
    `NiCamera::CalculateAndDrawShadowCasterLights`, i.e. before the shadow maps and long before the depth
    pass.
-   `BSShaderAccumulator::FinishAccumulating` (`0x1414b2240`) is `FinishAccumulatingPreResolveDepth` then
    `FinishAccumulatingPostResolveDepth`; for the main render mode the first of those (`FUN_1414b2d90`)
    **draws** the accumulated passes, over ranges of technique ids, reading the early-Z global at
    `0x14328cc79`. It does not assemble anything.

So the accumulator is complete early, and `globals::game::currentAccumulator` is a *currently rendering*
pointer that is simply not set yet. Re-probing against the accumulator latched from a frame where the
global was set:

| Hook point | via latched accumulator | via `currentAccumulator` |
| --- | --- | --- |
| `EarlyPrepass` (before the depth pass) | **612** | 0 |
| end of `Main_RenderDepth` | **612** | 0 |
| `Prepass` | 612 | 612 |

The passes were there the whole time.

### What was done

`BuildFrame` and the pipeline/program build move to `EarlyPrepass`, which runs from
`Main_RenderShadowMaps`. `CollectAccumulatedPasses` reads `SceneStore::latchedAccumulator`, refreshed by
`LatchAccumulator()` from `Prepass` where `currentAccumulator` is valid; a change of accumulator is logged
once. The first frame has no latch and builds empty tables, which is harmless.

`DrewLastFrame`'s one-frame tolerance absorbs the shift in when the frame counter advances, so the hybrid
skip needed no change.

### Two things that had to move back, and what caught them

Capture parity caught both; neither was visible in a screenshot.

1.  **`EyePosition` (VS PerGeometry variable 2): 134,700 mismatches of 157,800.** Most of what
    `SetupGeometry` writes is per frame, and `EyePosition` is relative to `posAdjust`. At `EarlyPrepass`
    the renderer's shadow state still belongs to the shadow-map camera just drawn, so the evaluation
    produced that camera's eye (113.07) where the main pass writes 0.
2.  **`EmitColor` (PS PerGeometry variable 8): 6-21 mismatches.** Candle and chandelier emissives flicker,
    and sampling `emissiveMult` at `EarlyPrepass` put it far enough from the draw that the parity check's
    0.1% relative tolerance on `EmitColor` no longer covered the difference (0.49074 against 0.49183, and
    1.2350 against 1.2546).

`SceneStore::RefreshFrameConstants()`, called from `Prepass`, re-evaluates the per-pipeline per-frame
constants and resamples the per-object shading against the main camera's state. To do that it needs the
property whose lighting pass supplied each pipeline's constants, so `tables.geometryTemplate` is kept
parallel to `tables.pipelines`. The object, geometry and material tables are camera- and
time-independent and stay in `BuildFrame`.

The Z-prepass epoch runs between the two and so uses the previous frame's values for those constants.
That is harmless: vertex position comes from `World`, which is patched per object at epoch time with that
epoch's own eye, and never from these.

**Gate:** capture parity **OK, 254,698 draws checked, 0 mismatched**; `CS_DCLF_BUILD_PARITY` OK on 959
sequences; the two epochs now report consecutive table generations (colour 5764, then the next frame's
z-prepass 5765) where they used to report the same one; no crashes, no device loss, 0 VUIDs.

## The object index reaches the shaders

Groundwork for the bindless step, landed and verified on its own.

`DrawSequence` now carries the object's index in `SceneStore::Tables::objects` as a third root constant
word, beside the two that already hold the `DrawBindings` address. The record grows from 64 to 68 bytes;
the push constant range and the `Constant` indirect argument both go from 2 words to 3, and because the
command signature's arguments are positional the vertex buffer, index buffer and draw arguments shift with
it.

Nothing reads it yet. It is the piece the shader needs in order to fetch per-object data from a
GPU-resident table instead of receiving it through a per-draw constant buffer, and it is what makes the
rest of the bindless work possible:

-   Of the PerGeometry group, only **five variables are per-object** - `World`, `PreviousWorld`,
    `MaterialData`, `EmitColor` and the w of `SSRParams` (this is the same finding the Step 2 template
    rests on). Everything else is per-pipeline.
-   Once the shader reads those five from a structured buffer indexed by this word, the PerGeometry
    cbuffer becomes *per-pipeline*, so the per-object arena allocations and packing disappear entirely.
-   With no per-object constant addresses left in it, `DrawBindings` becomes identical for every draw
    sharing a (material, pipeline) pair, so the records deduplicate and the per-draw upload collapses.

That last point is the one that matters for culling: the reason the whole tracked set cannot be handed to
the GPU today is that the CPU builds a binding record per candidate. When records are per-(material,
pipeline), feeding every candidate costs nothing per candidate, and the culling genuinely happens before
any per-draw work exists.

**Gate:** `CS_DCLF_BUILD_PARITY` reports 337 sequences matching the CPU templates byte for byte with the
new 68-byte layout, and the frame is unchanged. Only 21 sites in `Lighting.hlsl` reference those five
variables, so the shader change is small.

## Static ownership: capture at registration, then withhold

DCLF was a guest in the engine's per-frame loop. It now owns a set of objects outright: their passes are
built, lit and shadowed exactly as before, and then kept out of the main camera's batch renderer so the
native loop has nothing to draw.

### Capture belongs at `BSBatchRenderer::RegisterPass`, not at `GetRenderPasses`

The plan called for hooking `BSLightingShaderProperty::GetRenderPasses` (vfunc 0x2A). That cannot work:
the two fields the tables depend on do not exist when it returns. `AccumulatedPass::technique` is the
batch group's key, which the *caller* computes and hands to registration - it differs from the pass's own
`passEnum` because DoAlphaTest is added there - and `subPass` is chosen inside registration.

`BSBatchRenderer::RegisterPass(BSRenderPass*, std::uint32_t techniqueID)` (vfunc **0x02**, CommonLib
declares it) is the right place. It receives the technique as an argument, and it is also where
withholding belongs: not calling the original is exactly "the batch renderer never receives this pass".

`subPass` is derived rather than observed. The engine's classifier (`FUN_1414f4790`, called from
`RegisterPass` at `0x1414f2a20`) reads **only** the shader property's flags and the geometry's alpha
property: flag bit 54 alone puts a pass in list 4, otherwise the list is bit 36 as value 2, plus whether
`NiAlphaProperty::alphaFlags` bit 9 (alpha testing) is set. Nothing per-frame enters it, which is what
lets DCLF keep the field once passes stop reaching a batch renderer at all.

**Gate:** 634 registrations captured, 634 compared against the accumulator walk, **0 missing, 0 extra, 0
technique differs, 0 subPass differs**. The derivation matches the engine on every pass. The tables are
now built from the capture (`CS_DCLF_PASS_SOURCE=accumulator` restores the walk), and capture parity
stayed OK over 157,800 draws through the switch-over.

Incidentally, registration turns out to be **single-threaded** in practice, despite running under a job
list and despite the engine's classifier taking a mutex. The capture buffer is lock-free anyway, and
reports the thread count so a change would be noticed.

### Withholding

`CS_DCLF_OWNERSHIP=static`. The hook consults an immutable claim set, published whole once per frame and
read from whatever thread registers; a claim is a standing statement that DCLF owns an object, not the
per-frame decision the old `drawnFrame` skip was.

The claim is **what the colour epoch actually drew**, not what DCLF would like to draw. Withholding means
the native loop will not draw it either, so claiming something DCLF then fails to draw - a pipeline still
compiling, a texture not resolved - leaves a hole. Having drawn it once is the evidence that it can be
drawn again. Claims are dropped across a load screen, because they name geometry from the cell being torn
down.

Only the main camera's batch renderers are affected; the shadow cameras keep their passes. The native
**depth** pass is also untouched, because it registers through a different virtual
(`BSLightingShaderProperty::GetRenderDepthPass`, `0x1414aff30`) into a different renderer - so DCLF's
existing hybrid depth skip still does that half.

### The hole detector, and what it caught

Per frame on the CPU, with no readback: an object that was **withheld and not drawn** is a hole.

Being claimed is not sufficient on its own, and the first version got this wrong - it counted every
claimed object that went undrawn, which over-reported badly at cell transitions (6, then 14 "holes"). An
object the engine culled this frame is never registered, so it is never withheld either, and nobody was
going to draw it. The test for "the engine would have drawn it" is that its pass was captured this frame.

**Gate:** Bannered Mare, then `coc` to Whiterun, then to Dragonsreach - 921 passes withheld in steady
state, **0 claimed but not drawn** at every report including across all three transitions; no crashes, no
device loss, 0 VUIDs; the native opaque pass now has nothing left to skip (`0 in the opaque pass`).

### What this unblocks

The Phase 4 defect was that the native loop had already skipped an object by the time the depth segment
ran, so anything phase 1 rejected got no native draw, no DCLF depth and therefore no DCLF colour. With
ownership there is no skip decision to get wrong: the engine never draws a claimed object, so DCLF
culling one is simply correct. The Z-prepass no longer gates on `DrewLastFrame` under static ownership.

## Two-pass culling, switched back on

`CS_DCLF_CULL` had been unset since Phase 4, because a culling verdict could not mean anything while the
native loop decided ownership a frame late. With the table generations aligned (Step 3) and the engine no
longer drawing what DCLF owns (static ownership), it runs.

### Results

Dragonsreach, `CS_DCLF_OWNERSHIP=static`, `CS_DCLF_CULL=occlusion`:

- 1949 candidates tested, 835 rejected (42.8%): 159 outside the frustum, **676 occluded**.
- **158 of the occluded were objects the engine had kept** - the win the HZB exists for, since the
  engine's occlusion is planes, boxes and portals.
- Phase 2 brought back **75** objects the stale HZB had rejected and drew depth for 23 of them, so the
  two-phase rescue is doing its job.
- HZB: 1795 footprints sampled, 0 all-near, 264 all-far.
- **0 claimed but not drawn**, 0 VUIDs, no crashes.

Whiterun exterior, occlusion against a frustum-only control at the same place and hour:

| | Draws written | Rejected |
| --- | --- | --- |
| Frustum only | 337 | 162 frustum |
| Occlusion | **232** | 162 frustum + 250 occluded (210 the engine had kept) |

A 31% cut in DCLF's draws, and the two frames are equivalent - nothing is missing from the occluded one.

### Why claiming on "drawn last frame" does not fight the culling

There was a real worry here: claims are published from `drawnFrame`, so if culling an object stopped it
being drawn, the claim would lapse, the pass would stop being withheld, and the engine would simply draw
it again - correct output, no saving.

It does not happen, and the measurement says so rather than the argument. `claim churn: +0 -0` in steady
state with occlusion on. The reason is that `drawnFrame` records what the **CPU submitted**, not what the
GPU rasterised: the epoch loop marks every object it builds a record for, and culling happens afterwards
in `BuildDrawsCS`. So an occluded object stays owned and simply is not rasterised, which is what should
happen. The churn counters stay in the stats as a guard, since the property is not obvious from the code.

The same distinction is what keeps the hole detector meaningful: it reports objects DCLF failed to submit,
not objects it deliberately culled.

## Step C: the per-object constants leave the constant buffer

The five PerGeometry variables that differ between the objects of one pipeline - `World`,
`PreviousWorld`, `MaterialData`, `EmitColor` and the w of `SSRParams` - now come from a GPU-resident
table indexed by the object index the draw already carried, instead of from a constant buffer packed per
draw. Built behind `CS_DCLF_BINDLESS=1`, default off.

### How the shader reaches its own index

The draw's push data was already three words: the binding record's address, which the pipeline layout
consumes to resolve the draw's buffers and descriptors, and the object's table index, which nothing read.
It turns out no new binding kind is needed to read it.

BasicRHI's descriptor-heap path gives every push constant range a `VkDescriptorSetAndBindingMappingEXT`
of its own, sourced from `VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_DATA_EXT` at the range's `set` and `binding`
(`rhi_vulkan.cpp`, the loop over `layout.pushConstantRanges`). The `set`/`binding` fields are documented
in `rhi.h` as "ignored on Vulkan", which is true of the classic path and not of this one. So a shader
that declares an ordinary `cbuffer` at the range's register reads push data directly:

```hlsl
cbuffer DCLFPushData : register(b190)
{
	uint2 DCLFRecordAddress : packoffset(c0.x);  // consumed by the layout, not read here
	uint DCLFObjectIndex : packoffset(c0.z);
};
```

The object table itself rides the existing `DrawBindings::textures` mechanism at t127, so it needs no new
binding kind either. The one layout change is a vertex-stage range for that single register - the pixel
stage already had the whole t range, and the vertex half needs exactly one SRV. `ResourceDescriptorHeap[]`
was not needed after all, so the spike the plan reserved for it is not owed.

### The record

```hlsl
struct DCLFObjectRecord
{
	float4 World[3];          // row_major float3x4, already relative to the eye
	float4 PreviousWorld[3];
	float4 MaterialData;
	float4 EmitColor;         // emissive in xyz, the per-object w of SSRParams in w
};
```

128 bytes, and its second half is `ObjectShading` unchanged, which is what lets it be copied straight
through. The five names are reintroduced as `static` globals initialised from the table, so the 21 sites
that read them are untouched; `SSRParams` stays in the constant buffer, because only its w is per-object
and it is read at exactly one site.

`BuildObjectRecord` fills a record from the same inputs `PatchObjectGeometry` writes into a packed group,
through the same unwritten-component rule, and the table is filled for **every** candidate rather than
only the drawn ones - a candidate skipped by the epoch still reaches the culling, and an index that
addressed nothing would be worse than one that addresses a record no draw reads.

### The gate is a CPU comparison, not a screenshot

`CS_DCLF_BINDLESS_PARITY=1` compares each record against the constant group the non-bindless path packs
for the same object, component by component and bit for bit - a tolerance here would only hide a layout
mistake. It needs no readback and does not need both forms in one run, which matters because the
permutations are compiled against the switch and the two cannot coexist in one process.

**It held:** 0 of ~500 million components differ, across the Bannered Mare at hour 22 and the Whiterun
exterior. The frame is correct, static ownership reports 0 claimed-but-undrawn, and the culling still
reports 0 false negatives.

The second half of that gate is structural rather than counted: under `DCLF_BINDLESS` the vertex stage
stops declaring the PerGeometry buffer at all - SPIR-V binding 2 is simply absent from the module - so a
frame with objects in the right places is proof that the transforms came from the table.

### Then the buffer becomes per-pipeline

With nothing per-object left in the group, the objects of a pipeline share one pair of arena blocks
written once, and the per-object allocate/copy/patch disappears. Bannered Mare, per epoch:

| Part | Per-object buffer | Per-pipeline buffer |
| --- | --- | --- |
| Textures and samplers | 0.82 µs/draw | 0.25 µs/draw |
| **Constant groups** | **0.82 µs/draw** | **0.33 µs/draw** |
| Binding record | 0.09 µs/draw | 0.08 µs/draw |
| **Total per draw** | **1.96 µs** | **1.47 µs** |

Upload fell from 1.8 MB to 1.1 MB an epoch, and that is *after* adding the object table, which costs 128
bytes per candidate and lands in `rest` at no measurable change (0.76 ms against 0.77 ms).

What remains of `constant groups` is the material, light, permutation and alpha blocks, which were
already deduplicated and are not what this step was aimed at.

A measurement trap worth recording: with `CS_DCLF_BINDLESS_PARITY=1` the epoch still reported 0.85 ms of
constant groups, because the check repacks the group per object inside the timed region. The switch is a
gate, not a thing to measure through.

### What is left: deduplicating the binding record

The plan's third item - `DrawBindings` becoming identical for every draw sharing a (material, pipeline)
pair, so the records deduplicate and the per-draw upload collapses - is **not** reached yet, and the
reason is specific. Two entries of the record still vary per object:

-   the light block, keyed on (room index, shadow bit mask);
-   the alpha threshold block.

Both are constant buffer addresses, so the record differs whenever those differ. Moving them into the
object table is the remaining work, and it is a larger change than this one because the room index and
shadow mask are read by Light Limit Fix's own `StrictLightData`, not by `Lighting.hlsl` alone.

## Step D: the binding record deduplicates

The 800-byte `DrawBindings` record was assembled and uploaded once per draw. It is now one per
(material, pipeline) pair: **1616 candidates in Dragonsreach build from 105 records**, and the epoch's
upload falls from 0.9 MB to 0.5 MB. Behind `CS_DCLF_BINDLESS_DRAW=1`, which implies `CS_DCLF_BINDLESS`.

### Four blocks varied per object, not the two previously recorded

| Register | Keyed on | Where it is read |
| --- | --- | --- |
| PS b3 | `(roomIndex, shadowBitMask)` | `LightLimitFix::IsLightIgnored`, one call site in `Lighting.hlsl` |
| PS b11 | alpha threshold | one site |
| PS b8 | emissive multiplier | `Color::EmitColor`, one site |
| VS/PS b4 | `(pipeline, extra)` | - |

Three of those turned out cheaper than they looked:

-   **The permutation block's per-object variation is dead.** The only per-object bit DCLF puts in it is
    `SuppressExternalEmittance`, and that bit is read at exactly one place in the whole shader tree,
    `Effect.hlsl`'s `GetLightingColor` - never by `Lighting.hlsl` or anything it includes. So the block
    keys on the pipeline alone with no shader change at all. The non-bindless build keeps the bit, so
    capture parity goes on comparing the real thing against the engine.
-   **The 1216-byte light block was 16 useful bytes.** DCLF writes `{0, roomIndex, shadowBitMask, 0}` and
    nothing else, so `NumStrictLights` is 0 and the `StrictLights[15]` tail is never read. With the two
    live values moved, b3 is one zeroed block for the whole epoch.
-   **The `NSCB_ALIAS` shim made the b3 move nearly free.** `RoomIndex` and `ShadowBitMask` already
    resolve through `static const` aliases under DXC, so two alias lines become statics reading the object
    record. The cbuffer keeps its declaration and its layout, and Effect, Particle, Water and RunGrass -
    which include the same header and never define the macro - are untouched.

The record grew from 128 to 144 bytes to carry the four values. They went in a row of their own rather
than the spare `ObjectShading::materialData[3]`: that struct is shared with `ObjectGeometryConstants` and
`PatchObjectGeometry`, so anything parked there would leak into the non-bindless build's `MaterialData.w`
and into the parity comparison of that group.

`Lighting.hlsl`'s `DCLF_BINDLESS` block moved into `Common/DCLFObjects.hlsli`, because `Color.hlsli`
consumes the emissive multiplier and is included before the point where the block used to sit.

### Three defects this uncovered, all pre-existing

The capacity check `if (records.size() >= kMaxDraws)` was doing four jobs, and two of the others were
already wrong:

-   **`inputs` could overrun.** The depth epoch's cull-only push happened *before* that check, so the
    input count was bounded by the tracked-set size rather than by the buffer. Past 16384 tracked objects
    the upload would write beyond it and BuildDraws would dispatch over inputs never uploaded.
-   **`visibility` was indexed out of bounds**, by table index into a buffer sized for draws.
-   An object past the record table's capacity still got a draw, whose object index read zeros - a world
    matrix of zeros collapses it to a point at the eye with no other sign.

Both buffers are now sized by the table, and the caps are explicit and separate: object index against the
table, inputs against their own buffer, sequences against the draw cap, records against their own with a
`record-capacity` skip reason of their own.

Two more would have made the validation lie rather than fail:

-   **`CS_DCLF_BUILD_PARITY` sorted both sides by `bindingsAddress`.** Once records are shared that is not
    a total order, and equal-key runs land in arbitrary relative order on the two sides. It now sorts by
    object index, which is unique per sequence. While fixing it, a second limitation showed up: the check
    compared position by position, so with culling on it reported every sequence past the first culled
    object as differing. What the GPU writes is a *subset* of the CPU's templates, so it is now a
    subsequence check - and works for the first time on the configuration DCLF actually ships.
-   **The upload guard covered three buffers with one condition.** A depth epoch where every candidate is
    cull-only has no records and plenty of inputs, and BuildDraws would then run over a stale buffer.

### A parity check that was itself wrong

`emissiveMult` moved from a per-draw scene-graph dereference in the epoch onto the tables, beside the
shading that `MakeShading` already reads it for. The natural check was the record against a live read of
the property, and it fired: about 100 components in half a billion, always on flickering emissives.

**The check was what was wrong.** The multiplier is animated, and the shader divides it back out of the
emissive colour before re-applying it, so the record's multiplier has to be the *same sample* that
produced that object's `EmitColor` - which is why `MakeShading` now hands both back together. A live read
at epoch time is a strictly later sample, and matching it would have broken the cancellation rather than
proved anything. What is checked instead is the plumbing: `emissiveMult` is a new parallel array, and a
gap in one of those shifts every later object's index. Whether it is sampled at the right point in the
frame is already covered, and covered better, by capture parity's `EmitColor` comparison against the
engine's own draw.

### Gates

-   **`CS_DCLF_DEDUP_PARITY=1`**, the check that matters: rebuild the record per draw and compare it byte
    for byte against the one its pair holds. **3,981,022 rebuilt records, 0 differ.** This is the only
    check that would catch a fifth per-object dependency nobody had noticed.
-   `CS_DCLF_BINDLESS_PARITY` extended to the four new values: clean.
-   `CS_DCLF_BUILD_PARITY` OK with culling on, 825 of 921 sequences matching with 96 rejected.
-   0 claimed-but-undrawn, 0 VUIDs, correct frames in the Bannered Mare, Whiterun exterior and
    Dragonsreach - which between them exercise all three moved registers: alpha-tested banners, emissive
    fires, and portal-strict interior lighting.
-   Offline, before any run: `DCLF_BINDLESS_DRAW` removes bindings 8 and 11 from the pixel module and
    keeps 3, which is reflection-level proof the registers really left the shader.

### What it cost and what it bought

Bannered Mare and Dragonsreach, per epoch:

| | Before Step C | After Step C | After Step D |
| --- | --- | --- | --- |
| Per draw | 1.96 µs | 1.47 µs | **0.74 µs** |
| Records per epoch | one per draw | one per draw | one per (material, pipeline) |
| Upload | 1.8 MB | 1.1 MB | **0.5 MB** |

Distinct pairs measured 85 to 201 across three areas, so the record buffer is sized at 2048 entries
(1.6 MB) rather than 16384 (13.1 MB) when deduplication is on, and stays at the draw cap when it is not.

**The structural result matters more than the microseconds.** The reason the whole tracked set could not
be handed to the GPU was that the CPU built a binding record per candidate. It no longer does, so feeding
every candidate to the culling now costs a draw input and a sequence - and nothing else.

## Per-object work, Stage 0: the measurement was lying

`BuildFrame`'s cost was reported in four parts, one of which was called "classification" and read 1.272
ms a frame in Dragonsreach. It was the largest single DCLF cost and the obvious next target.

It was not one thing. `PartTimer` is called at the *top* of each iteration, so that bucket accrued
everything since the previous object: both `FindAccumulatedPass` lookups, `ClassifyStatic`,
`ClassifyFrame`, the derivation counters, two `GpuResources::Resolve` calls, three dedup probes, two
transforms, `ExternalEmittance::ShouldSuppress`, `GetRoomIndex`, `MakeShading`, the `objectIndex` insert
and the `DrawSequence` build. Nine things under one label.

The parts are now ten, each measuring one thing, behind `CS_DCLF_PROFILE=1` (default off, so the loop
makes no clock calls in a normal run). Whiterun exterior, 9022 tracked / 2885 eligible:

| Part | ms | | Part | ms |
| --- | --- | --- | --- | --- |
| **record** | **1.444** | | resolve | 0.199 |
| classify-static | 0.907 | | diagnostics | 0.118 |
| classify-frame | 0.532 | | dedup | 0.051 |
| pass-lookup | 0.395 | | pipeline-eval | 0.045 |
| material-eval | 0.208 | | walk | 0.129 |

**The largest part is not classification.** It is `record` - the object record assembly: transforms,
bounds, flags, emittance, room index, shading and the draw. Classification is second, and its two halves
together (1.439 ms) only just match it.

The two scale differently, which is what matters for coverage:

-   **classification scales with the tracked set** (9022 objects, ~160 ns each). It is paid for every
    object whether or not it is ever drawn - including the 6097 the exterior rejects every frame.
-   **`record` scales with the eligible set** (2885 objects). It is the cost of actually drawing.

So classification is the part that grows as coverage grows, and `record` is the part that grows as
coverage *succeeds*. Both need work; the priority between them was not what it looked like.

One caveat the numbers carry: timing a loop from inside it perturbs it. Profiled, the exterior reads
4.035 ms; unprofiled it is 2.855 ms. The instrument costs 41%, so the parts are upper bounds and only
their ranking should be trusted.

## Stage 1: dead and duplicate work

No caching yet - only removing work that was being done twice or for nobody.

-   **One pass lookup per object, not two.** The ordering pass called `FindAccumulatedPass` to split
    engine-kept objects from culled ones, then the loop called it again for the same object. The pointer
    is now carried in the ordering entry; nothing mutates the map while the loop runs.
-   **One pass map per frame, not three.** `CollectAccumulatedPasses` walked the batch renderers to fill
    `accumulatedPasses`, `CompareCapturedPasses` built a second map of the same size to compare against
    it, and then the first was discarded and refilled from the second. The capture now fills the table
    directly; the walk and the comparison run only under the new `CS_DCLF_PASS_PARITY=1`, or as the
    first-frame fallback. The batch-renderer set the capture hook needs is refreshed separately, since
    that part *is* needed every frame.
-   **`RefreshCategoryNodes` no longer runs every Present.** It rebuilt and diffed the whole category set
    on every frame, with a full linear scan over the tracked map whenever any node had gone, to detect
    something that changes only when a cell attaches or detaches. It now runs when a cheap signature
    changes, when a detach was drained this frame, or on a 30-frame backstop. The signature deliberately
    folds in everything the refresh reads to *find* nodes - the interior cell, the grid cells, each
    cell's loaded data, its cell3D and its child count - because missing a change means geometry attached
    under a new node is never tracked at all.
-   **Frame-globals hoisted out of the loop.** `Util::IsInterior()` was being evaluated per object inside
    `ExternalEmittance::ShouldSuppress`; that function gained an overload taking the answer, so the
    interior test happens once a frame. Same for the two feature-loaded flags.
-   **The derivation counters are computed only when they are reported.** The whole block, including a
    popcount loop, existed to feed one log line that only `CS_DCLF_STATS` prints, and ran per object per
    frame regardless.
-   **Two vectors stopped being reallocated.** The ordering vectors were locals, so a 9000-object
    exterior allocated and freed two ~140 KB buffers every frame; and the engine-kept count was taken in a
    second pass over the finished table rather than as the objects were built.

### One item was dropped, and the reason is the interesting part

The plan called for removing the duplicated `MakeShading` in `BuildFrame`, on the grounds that
`RefreshFrameConstants` overwrites it at Prepass. It does - but **the Z-prepass epoch runs between
them**, and under `DCLF_BINDLESS` it reads `MaterialData` out of the per-object record, which comes from
exactly that write. `Lighting.hlsl` uses `MaterialData.z` for the alpha test at lines 2833 and 2840,
*before* the `DCLF_DEPTH_ONLY` return at 2857. The same argument applies to the duplicated
`EvaluateGeometry`: `RefreshFrameConstants` deliberately keeps `BuildFrame`'s value for a pipeline whose
template has no lighting pass.

Neither is dead. They are the depth epoch's copy, and the plan's "carefully" was warranted.

### Result

| | Before | After |
| --- | --- | --- |
| Whiterun exterior | 2.855 ms | **2.479 ms** |
| Dragonsreach | 1.676 ms | **1.531 ms** |

**Gates:** `CS_DCLF_PASS_PARITY` reports 1127 compared, 0 missing, 0 extra, 0 technique differs, 0
subPass differs (with ownership off - with static ownership the walk sees only what is not withheld, so
921 show as "extra", which is the whole point of capturing at registration instead). Capture parity 0
mismatched over 276,300 draws. Object, geometry, pipeline and material counts identical across three
cells and two transitions; 0 claimed-but-undrawn; 0 VUIDs.

## Stage 2: why the depth pass is not owned, measured

Owning the depth pass would delete `SkipNativePass` and the per-draw `drawnFrame` map. The plan assumed
it was the same mechanism as the opaque pass with a different virtual. It is not, and a probe on
`RegisterPass` says so directly (`CS_DCLF_REGISTER_PROBE=1`, Dragonsreach, per frame):

| Shader type | Registrations | Into a main-camera batch renderer |
| --- | --- | --- |
| Lighting (6) | 1125 | **1125** |
| Utility (8) | 2396 | **3** |

`BSLightingShaderProperty::GetRenderDepthPass` builds its pass with **`BSUtilityShader`**, not the
lighting shader, and those passes go into batch renderers that are not the main camera's. Withholding
filters on `mainBatchRenderers`, so it never sees them.

Owning them means telling the main camera's depth renderer apart from the shadow cameras', which is most
of that 2396 - and withholding a shadow camera's pass removes the object from every shadow it casts.
Against that, the measured prize is ~921 hash lookups a frame, on the order of 0.05 ms. **Deferred, with
the evidence recorded rather than the assumption.** The probe stays behind its switch.

## Stage 3: the classification cache is correct, and buys almost nothing

The plan called this "the main event": in the Whiterun exterior 6097 of 9022 tracked objects run a full
classification every frame to produce "no", and are discarded. Caching that verdict should have removed
most of `classify-static`.

It did not.

| | ms |
| --- | --- |
| `classify-static`, no cache | 0.952 |
| `classify-static`, 5959 objects served from the cache | 0.847 |
| Whole table build, exterior | 2.479 -> 2.460 |

About 17 ns saved per cached object, against the ~160 ns the part costs per tracked object. The reason is
that the work being cached was cheaper than it looked: `netimmerse_cast` is a walk up a chain of RTTI
*pointers*, not a string comparison, and most negatives never get that far - `NotTriShape` and `Skinned`
are decided in the first three lines. What remains in `classify-static` is the witness read itself, which
has to happen for every tracked object either way.

### The part that matters more than the code

The negative cache helps objects that are **ineligible**. As coverage grows to new object classes,
objects move from ineligible to eligible - so this cache helps *less* after coverage grows, not more.
That is backwards from the goal it was written for.

What grows with coverage is the other two:

-   **`record`** (1.218 ms, the largest part) - the object record assembly, paid per *eligible* object.
-   the **positive** half of the derivation, which cannot be cached as it stands because its result comes
    from the engine's per-frame render pass.

So the work that makes per-frame cost fall as coverage rises is the record assembly and the positive
path, not the negative verdict. That is a finding about the plan, not about the code, and it is the
reason to record it here.

### What was kept, and why it is narrow

The cache is on `Tracked`, validated by four pointer witnesses (renderer data, shader property, material,
and a one-byte fade state) compared in full every frame. Only four verdicts are cached:
`NotTriShape`, `Skinned`, `NoRendererData` and `NotLightingShader` - the ones that follow from the
geometry's own shape, which is exactly what the pointer witnesses cover.

Everything decided later is deliberately excluded, because its inputs change behind an unchanged pointer:
`AlphaBlend` reads `materialAlpha`, which is animated; and the reasons `DeriveLightingDescriptors`
produces read property *flags*, which Community Shaders' own features set in place. Caching those would
have traded a real correctness surface for the cheapest third of the population. The first version did
cache them, and the restriction is the more defensible code even though it halves an already small win.

**Gate:** `CS_DCLF_CLASSIFY_CACHE=probe` serves the cached verdict *and* recomputes it, comparing the two.
571 recomputed and compared per frame, **0 differ**, across the Bannered Mare, the Whiterun exterior and
Dragonsreach with two transitions; table counts identical; 0 claimed-but-undrawn; 0 VUIDs.

### The fade metric, reduced to one byte

`FadeStateOf` is the reusable part of this stage. The LOD metric feeds the derivation in exactly three
ways - reject on `!isfinite`, clear `kSpecular` past its fade end, clear `kEnvMap` past its - so two bits
and an invalid marker capture the whole camera dependence of the classification. An object with none of
the fade-sensitive flags returns 0 without touching the fade node at all. The fade *floats* the metric
also produces are overwritten from the property by `RefreshFrameConstants` before any draw reads them, so
they are not part of the witness.

## Switches

| Variable | Effect |
| --- | --- |
| `CS_DCLF_STATS=1` | Every 300 frames, log how many objects are tracked, why the rest stay native, and the CPU time scene capture takes. |
| `CS_DCLF_CAPTURE_PARITY=1` | Compare the tables with the native draws (see above). Costs CPU on every draw. |
| `CS_DCLF_DEBUG_VIEW=1` | Before the deferred composite, copy DCLF's off-screen targets over the native ones: the frame shows only what the indirect draws produced. |
| `CS_DCLF_HYBRID=1` | DCLF draws into the main pass's own targets and depth, and the native loop skips the objects it drew. |
| `CS_DCLF_HYBRID_NOSKIP=1` | With the hybrid path, keep drawing everything natively, so what DCLF fails to draw is still visible. |
| `CS_DCLF_ONLY_ELIGIBLE=1` | The reverse skip: the native frame draws only the objects DCLF draws, so the two can be compared pixel for pixel. |
| `CS_DCLF_CULL=off\|frustum\|occlusion` | How BuildDrawsCS filters the candidates before writing their sequences: nothing, the frustum, or the frustum and then the HZB. |
| `CS_DCLF_CULL_INPUT=native\|tracked` | Which candidates may be drawn: only what the engine's culling kept (the default), or whatever the GPU culling keeps. `tracked` still shades incorrectly (see Phase 4). |
| `CS_DCLF_BUILD_PARITY=1` | Every 300 epochs, read BuildDraws' output back and compare it with the CPU templates (`BuildDraws parity OK/MISMATCH`). |
| `CS_DCLF=0` | Turns the feature off entirely: no hooks, no tables, no draws. Anything else, including unset, leaves it on. |
| `CS_DCLF_TABLES=tracked\|accumulated` | Whether the tables hold the whole tracked set or only what the engine's accumulator kept. `accumulated` was the fallback while the per-pipeline template defect was open; `tracked` is now correct. |
| `CS_DCLF_EVAL=off\|material\|geometry` | Diagnostic: suppresses parts of the stand-in evaluation. Note that a suppressed evaluation also stops its objects being drawn, so a clean frame under it proves nothing on its own. |
| `CS_DCLF_OWNERSHIP=static` | Withhold claimed passes from the main camera's batch renderer, so DCLF owns those objects outright. Default off. |
| `CS_DCLF_PASS_SOURCE=accumulator` | Build the tables from the accumulator walk instead of the captured registrations. |
| `CS_DCLF_MATERIAL_CACHE=probe` | Diagnostic: measures how many material records are unchanged from the previous frame, i.e. whether a cross-frame cache could work. |
| `CS_DCLF_EVAL=audit` | Diagnostic: snapshots pipeline state around every stand-in call and reports anything not restored. Very slow; the frame rate collapses. |
| `CS_DCLF_TEST_COMMANDS=<frame>:<command>;…` | Test runs: run each console command on the main thread once that many frames have been presented (loading screens do not count), for coverage runs from the auto-loaded save. The counter is independent of the feature, so a `CS_DCLF=0` control reaches the same place at the same hour. |

## Known upstream issues

Found while building DCLF. None is caused by DCLF; each is recorded here until it is fixed or ruled out.

| Where | Issue | Status |
| --- | --- | --- |
| DXVK fork (`extern/dxvk`) | Intermittent validation error `VUID-vkQueueSubmit2-semaphore-03868`: a binary semaphore in a submit's signal list is still signaled. 0 to 5 per run, in the swapchain/present path. It started in this branch's runs; DCLF issues no Vulkan calls. The newest `extern/dxvk` commits include a presenter revert, the suspected cause. | Open, not investigated |
| Community Shaders, Subsurface Scattering | `ExtraShaderDescriptors::IsBeastRace` is set or cleared only in the face/skin `SetupGeometry` path (`SubsurfaceScattering::BSLightingShader_SetupSkin`), but `Lighting.hlsl` writes `!IsBeastRace` into the G-buffer mask (`psout.Masks.y`) for every object. After a beast-race face draw, the static objects drawn next inherit the bit. DCLF uses the intended value (0), so once DCLF draws them, its output can differ from native in that mask channel. | Open; not seen in the coverage runs (no beast-race faces in view) |
| Community Shaders, permutation buffer | `IsSun` and `GrassSphereNormal` stay set in `ExtraShaderDescriptor` from the last sky/grass draw. The Lighting shader does not read them, so this is harmless today, but any shader that starts reading them inherits stale values. | Harmless for now |
| Community Shaders, TruePBR | Constant arrays passed to the shader were only partly initialized (e.g. `ParallaxOccData` zw). | Fixed (`TruePBR.cpp`, value-initialized) |
| Community Shaders, Effects11 | Logs `[E] Required effect file not found: enbseries\enbeffect.fx` on every start without ENB files. | Unrelated noise |
| Skyrim, `BSLightingShader::SetupTechnique` | Writes `VPOSOffset` after unmapping the PerTechnique buffer. It works on DXVK, where dynamic buffers stay mapped. On a native D3D11 driver the write is undefined. | Engine behaviour; DCLF reproduces the value |
| Skyrim, fog constants | `FogFarColor.w` is copied from uninitialized stack memory. | Engine behaviour; the shader does not read it |
| CommonLibSSE-NG | `BSBatchRenderer::renderPass` is declared as `BSTArray<PassGroup*>` but holds `PassGroup` structs inline. | DCLF works around it; not fixed in the library |
| Tooling | The CS deploy step (`*_plugin.stamp`) sometimes fails on its first run and succeeds on the next. Once (`dclf16`) the game recompiled all 3,473 shaders despite logging "Using disk cache"; the cause was not found. | Open, cosmetic |

## Open questions and assumptions (Phase 1)

-   **DoAlphaTest gained after the tables are built.** Passes in alpha-test batch lists whose registered
    technique lacks DoAlphaTest are sometimes drawn with it (engine notes, batch renderer). The source of
    the change was not found. These objects stay native (`alpha-test-state`).
-   **Parentless interior geometry.** About 30 draws per frame in Dragonsreach and Bleak Falls Barrow have
    no parent node; probably the portal graph's `alwaysRenderChildren` or its other object lists. Not
    tracked, so these stay native.
-   **Emissive drift.** `EmitColor` differs from the drawn value by up to about 1e-4 relative on emissive
    windows: the time-of-day emittance moves between the table build and the draw. Where it is updated
    was not traced. Parity compares it within 0.1%.
-   **Shadow mask size.** `VPOSOffset` uses the shadow mask render target's size. The engine uses globals
    at AE `0x14328be9c`. The two were equal at 1920×1080; not checked with a different mask size
    (`iShadowMaskQuarter`) or dynamic resolution.
-   **Early-Z global.** Whether DoAlphaTest is set for alpha-tested geometry depends on which offscreen
    renders ran before (engine notes). Phase 5 must decide how to derive it.
-   **Stand-in side effects.** Evaluation calls `SetupMaterial`/`SetupGeometry` through the vtable, so every
    Community Shaders hook runs. The engine shadow state and CS's permutation data are restored.
    Light Limit Fix's hook uploads its `StrictLightData` for the template pass. It records what it
    uploaded, so its upload cache stays consistent, and the next native draw overwrites it. A hook added
    later that keeps other global state would need the same review.
-   **Template pass for geometry constants.** Per-pipeline `PerGeometry` constants are evaluated with a
    copy of any object's lighting pass (for the sun light). This is valid because everything per object
    is replaced afterwards (engine notes, `SetupGeometry`: what is per object); parity confirms it for the
    main pass only.
-   **Actors.** Geometry under an actor's 3D is excluded, including rigid items carried by actors.
-   **Unwritten constants.** Components the engine never writes are uploaded as zero in Phase 2. The
    engine leaves them undefined, and the permutations do not read them.
-   **SE 1.5.97 and VR.** Only AE 1.6.1170 was tested. DCLF uses two raw offsets, into
    `BSBatchRenderer`'s `renderPassMap` (`+0x2C`, `+0x48`); their SE layout is assumed identical. VR is
    disabled.

## Still open in Phase 1

-   Parentless geometry in interiors (about 30 draws per frame in Dragonsreach and Bleak Falls Barrow)
    stays native; probably the portal graph's always-render lists.
-   The alpha-test-state objects (engine notes, batch renderer open question) stay native.
-   **Cost:** see the results table; tracking adds about 0.05 ms. In Whiterun, 0.54 ms of the 1.05 ms is
    classification and record building (about 1,450 accumulated tracked geometries) and 0.38 ms is the
    stand-in evaluation of 295 materials. Caching materials across frames is the obvious next saving; it
    needs a reference on the material so that a reused address cannot hit a stale entry.
-   Phase 5 cuts the native loop, so the accumulator walk will no longer supply the per-frame technique
    bits. They then have to be derived: the light and shadow assignment (`FUN_1414fcf80`) and the early-Z
    global.

## Stage 4: the per-object loop stops repeating itself

Stage 0 named `record` the largest part of `BuildFrame`. Splitting it showed the label was wrong in the
same way Stage 0's had been. `timer.Add(BuildPart::Dedup)` fired only inside the miss branches of the
three dedup maps, so on the hit path — nearly every object — three hash probes fell through to the next
iteration and were billed to `record`. Whiterun exterior, 3057 objects:

| part | ms |
| --- | --- |
| `record` (the real assembly) | 0.615 |
| `dedup-hit` (the three probes) | 0.396 |
| `loop-tail` (per *tracked* object) | 0.336 |

Half of what `record` reported was not record assembly. The named work was nearly free in that cell
anyway: `GetRoomIndex` early-outs on `roomNodes.empty()` outdoors and `ShouldSuppress` short-circuits on
`!interior`.

**Container churn.** The three dedup maps were locals — three hash maps allocated and freed every frame
to hold the same contents, the waste Stage 1 removed from the ordering vectors and left here. They are
members now, and `objectIndex`, `shading`, `emissiveMult` and `lights` are reserved like the other
per-object containers. The two ordering passes became one: `order` and `culled` were built separately
and then concatenated, up to a ~6000-entry memcpy a frame, purely to guarantee that an engine-kept
object reached a pipeline first.

**The pipeline template is elected, not ordered.** That guarantee is now stated as a rule about the
objects: if a later native-visible object finds a pipeline whose lighting template came from a culled
one, it takes the template over. This survives a table that outlives the frame, which ordering cannot.
The first gate statistic counted *pipelines with a culled template* and read 12 of 31 under
`CS_DCLF_CULL_INPUT=tracked` — a false alarm, because a pipeline drawn only by culled candidates has no
native-visible object to elect and harms nothing. The invariant that matters is the implication, checked
over the finished tables rather than asserted from the election: **if a native-visible object draws on
pipeline p, then `geometryTemplate[p]` came from a native-visible object.** Measured with the election
doing real work — 10 takeovers a frame — it holds at 0, and Dragonsreach lighting is unchanged.

### The material cache, and why the measurement lied twice

The plan called for promoting `materialProbe`, "a 99% hit rate that only needs promoting". It was not.

The probe compared records with `memcmp` over a struct whose members total 2340 bytes while the struct
is 16-aligned, so it was comparing **12 bytes of uninitialised trailing padding**; every comparison came
out unequal. With member-wise equality it still read 104 changed, 0 unchanged — but only the PS block
moved, at three adjacent floats holding the **same value for every material in the frame** (min == max
across all 104) and drifting with the time of day.

`BSLightingShader::SetupMaterial` explains it. In Ghidra it is `BSLightingShader::Func4` at `1414dc310`
— vtable slot 4, named by slot rather than by name. Its tail reads:

```c
if (*(char *)(this + 0xf0) == '\0') { lo = *(this + 0xe0); hi = *(this + 0xe8); }
else                                { lo = *(this + 0xd0); hi = *(this + 0xd8); }
offset = *(byte *)(currentPS + 0x5d);   /* PS constant table[29] */
```

PS PerMaterial **variable 29 is IBLParams** (`ShaderCache.h:50`), and SetupMaterial does not read it
from the material at all: it comes from fields of the BSLightingShader object, selected by a day/night
flag at `this+0xf0`. It is shader-level frame state that happens to live in the per-material group.

So the cache serves the record and patches those positions from a live evaluation. **The positions are
cumulative for the session**, which a failure taught: learning them each frame as "floats that differ
from the cached copy" works only while the value drifts continuously. `set gamehour to 22` steps it once
and then freezes it — the next frame saw no difference, learned an empty set, patched nothing, and
served every material its pre-step value. Capture parity failed on **220,500 of 276,300 draws**, with
IBLParams.y reading 0.757 against a native 0.106.

The values are sampled again at Prepass (`RefreshMaterialPatch`), for the same reason and in the same
place the animated per-object shading is resampled: sampled only at EarlyPrepass they sit a fraction of
a frame behind what the native draws read, visible as IBLParams differing in the sixth decimal across a
fast lighting transition.

The result is that the cache is parity-*better* than evaluating every material, because it fixes a
pre-existing mismatch the uncached path has. Mismatched draws per report interval, same route:

| interval | cache off | cache on |
| --- | --- | --- |
| cell change (`coc`) | 253239 | 20572 |
| `set gamehour` step | 248670 | **0** |
| steady state | 0 | **0** |

104 evaluations a frame became **9** — one to learn the patch, eight rolling validations — with 103
served. `material-eval` went 0.443 → 0.106 ms in the exterior and 0.045 in Dragonsreach. It defaults on.

**Two instrument defects, both of the same kind.** The validator's cursor reset every frame, so with a
stable iteration order it re-checked the same eight materials for ever and reported "0 stale" while
capture parity was failing on 80% of draws. And `probe` forced the evaluate path, so it measured a
configuration with no cache in it. Both now follow Stage 3's shape: serve the cached value, recompute
it, compare, and use the served one — a probe that does not exercise the thing it is probing proves
nothing.

### Buffers resolve once per TriShape

Both `GpuResources::Resolve` results were only ever read inside the `newGeometry` branch; for an object
whose geometry was already in the table they were computed and discarded. With ~3.8 objects per TriShape
in the exterior that is most of the calls, and in steady state a Resolve is a hash probe
(`DescribeResource` runs only on first insert).

The call was **moved**, not cached, and the distinction is the safety argument: GpuResources holds a
reference on each buffer so its address cannot be reused, and it drops that reference when an entry goes
`kEvictFrames` without a Resolve. Resolving per TriShape still touches every entry the tables depend on
every frame. Skipping the call outright would have let the eviction sweep drop the reference underneath
live draw arguments, which is the device-loss hazard `BuildFrame` already documents.

`resolve` went 0.326 → 0.176 ms in the exterior, 0.110 → 0.072 in Dragonsreach.

### The property derivation is dead work for an accumulated object

For an object the accumulator holds, `DeriveLightingDescriptors`' derivation block has exactly one
surviving effect: `a_out.derivedPass`. The pass descriptor comes from `a_accumulated->technique`; the
flag edits the block makes to `f` are never read afterwards; the two LOD fades it computes are
overwritten from the property two lines later; and its return value is not consulted to reject the
object. Only the derivation diagnostic reads `derivedPass`.

So for those objects the fade metric, `SelectLightingTechnique`, ten flag tests and a virtual
`GetFeature()` call were being run and thrown away. They are skipped now unless the diagnostic asks for
them, which also moved that diagnostic from `CS_DCLF_STATS` to its own `CS_DCLF_DERIVE_PROBE`: gating it
on the stats switch meant every reporting run measured a configuration nobody ships. The report line is
gated too, because printed unconditionally it read "0 objects compared, 0 differ", which scans as a
passing check rather than one that never ran.

The saving is smaller than it looks — `classify-static` 0.363 → 0.338 ms in Dragonsreach, and nothing
measurable in the Whiterun exterior. The reason is worth recording: in the exterior most candidates are
*not* accumulated (3057 objects, ~915 engine-kept), and for those the derivation **is** the pass
descriptor, so it has to run. The work removed scales with what the engine kept, not with what DCLF
tracks.

What remains in `classify-static` is not computation. A descriptor memo keyed on the pass descriptor was
tried in an earlier stage and measured neutral, and `ModifyShaderLookup` is pure bit manipulation on that
descriptor. The remaining ~300 ns per call is dominated by touching the geometry, property, material,
alpha property and fade node — five separate allocations, each a likely cache miss. Removing it means
not touching them at all, which is a *positive* verdict cache, not a cheaper derivation.

### The RTTI cast is resolved once per property, and that is where classify-static stops

`netimmerse_cast` is cheap in instructions — it walks a chain of RTTI *pointers*, not strings — but every
step is a dependent load into a different allocation. At ~3000 classifications a frame it was the one
part of `ClassifyStatic` that was neither computation a memo could remove nor memory `BuildFrame`
re-reads later anyway. Whether a property is a `BSLightingShaderProperty` follows from its type, so the
property pointer is a complete witness, and `Tracked` now remembers both the pointer and the result
(null included, which is what makes a negative trustworthy).

**RTTI casts walked per frame: 0.** `classify-static` 1.011 → 0.945 ms in the exterior, 0.363 → 0.312 in
Dragonsreach, with the classification probe still at 0 differ and capture parity clean.

That is ~11%, and it is the end of the road for this part. With the casts at zero and the derivation
skipped for accumulated objects, `classify-static` is still 0.945 ms over ~3060 calls — about 310 ns
each — and what remains is memory: the geometry runtime data, the property, the material, the alpha
property and the fade node, five allocations touched per object. A *positive* verdict cache cannot
recover much of it, because its own witnesses must read the geometry, the property and the fade node,
and `BuildFrame` re-reads the alpha property and the material downstream for the two-sided flag, the
alpha test and the material key. What is left is the pointer chasing the data layout implies, not work
that can be cached away.


### Result

**Dragonsreach 1.531 → ~1.31 ms** a frame, unprofiled, over ten steady-state intervals. Gates: capture
parity 0 mismatched over 276,300 draws an interval, classification cache 0 differ, pipeline templates 0
visible objects on a culled template, material cache 0 stale, 0 claimed-but-undrawn, and no VUIDs beyond
the upstream semaphore one.

**Still open.** Caching the *table slots* on `Tracked` — the remaining `dedup-hit`, 0.373 ms — needs the
tables to persist first, because a slot is only worth caching if it is stable. The plan had that
dependency the other way round.

## Stage 5, attribution: the epoch, and a switch that was off

Splitting `BuildFrame` twice had shown the same failure both times — a bucket named after one thing that
mostly contained another. The epoch's four parts had never been split at all, and "rest" was a
subtraction. Splitting it found two things, the second of which matters more than any optimisation in
this document.

### The parts are per record, not per draw

`mark(0..2)` all sit inside `if (!dedup || recordIndex == kNoRecord || dedupParity)`. Since Step D
deduplicated the binding record to one per (material, pipeline) pair, that block runs ~105 times an
epoch rather than ~1500. So "textures/samplers", "constant groups" and "binding record" were per
*record*, while the per-*draw* work — the loop prologue with its `resolvedBindings` probe, and the tail
that builds the sequence and the draw input — was invisible inside "rest", together with the uploads and
the graph execution.

### Step D's deduplication was never switched on

The split's first measurement read `1538 candidates built from 1538 binding records`. `dedup` is
`BindlessDraws()`, which requires `CS_DCLF_BINDLESS_DRAW`, and that defaults off — so **every
measurement taken since Step D has had its deduplication disabled**, and "constant groups 0.715 ms" was
~465 ns of constant packing per draw rather than per pair.

With the switch on, in Dragonsreach, 1521 candidates collapse to **105 binding records**:

| part | dedup off | dedup on |
| --- | --- | --- |
| textures/samplers | 0.131 | 0.047 |
| constant groups | 0.715 | 0.162 |
| record push | 0.160 | 0.060 |
| per-draw tail | 0.117 | 0.102 |
| per-draw prologue | 0.188 | 0.118 |
| epoch prologue | — | 0.052 |
| graph execute | — | ~0.61 |
| **epoch total** | **~1.98** | **~1.15** |

That is ~1.6 ms a frame across the two epochs, from a feature already written, tested and gated. Its
gates hold: record dedup parity 4.8M rebuilt records matching their pair byte for byte, bindless record
parity 204M components matching the constant groups, no mismatches, no VUIDs beyond the upstream
semaphore one, and Dragonsreach renders identically.

It is **not** defaulted on here, because the standing debt lists a live test matrix — kill cams, the map
menu, first person, water reflections, Dynamic Cubemaps, save and load — that has to be worked through
before any switch flips, and `CS_DCLF_BINDLESS_DRAW` changes the shader build as well as the CPU path.

### What is left is not per-object

Fully split, with dedup on:

| part | ms | scales with |
| --- | --- | --- |
| **graph execute** | **~0.61** | **nothing — fixed per epoch** |
| constant groups | 0.162 | records |
| per-draw prologue | 0.118 | candidates |
| per-draw tail | 0.102 | candidates |
| record push | 0.060 | records |
| textures/samplers | 0.047 | records |
| epoch prologue | 0.052 | nothing |
| uploads | 0.000 | deferred to the graph's upload pass |

Half the epoch is the graph execution and the `PassFrame` build — fixed cost, independent of object
count, and paid twice a frame. The genuinely per-candidate part is the prologue plus the tail: about
**145 ns per candidate per epoch**, not the ~1.2 us a division of the old total implied.

## Stage 5: the transform witness cannot pay for itself

The plan's Stage 5 rested on one idea: most of a Skyrim cell never moves, so a 48-byte transform compare
should skip the record write and the upload for most objects. Stable object slots — a free list, one
array indexed by slot, per-slot frame stamps, every table consumer changed — existed mainly to make that
possible.

Three measurements say it does not work, and none of them needed the slots built.

**The transform stores are 13 ns an object.** `CS_DCLF_TRANSFORM_PROBE=skip` removes both
`StoreTransform` calls outright — it renders wrong, and exists only to bound the cost. In Dragonsreach,
1962 objects: `record` 0.413 → 0.387 ms. That 0.026 ms is the *ceiling* on anything a transform cache
could save. A witness would read two 52-byte `NiTransform`s and copy 48 bytes to avoid 18 multiplies, so
it adds more memory traffic than the arithmetic it removes. It would be a net loss.

**The record build is 26 ns an object.** `BuildObjectRecord` runs for every object before the epoch loop,
and that whole prologue measures 0.052 ms for 1962 objects.

**The upload is already free on the CPU.** `BUFFER_UPLOAD` reaches `UploadService::UploadData`, which
records the pointer and size; the copy happens in the graph's upload pass. The measured "uploads" part is
0.000 ms. The copy is real, but it lands inside `graph execute`, and turning the bindless object table
on and off — 282 KB an epoch — moves `graph execute` by less than run-to-run noise.

So the three things Stage 5 was going to avoid cost 13 ns, 26 ns and approximately nothing. **Stable
object slots are not worth building for this.** They may still be worth building for a different reason —
they would remove the `objectIndex` insert, which `dedup-hit` measures at ~119 ns a draw — but that is a
different argument from the one in the plan, and it should be made on its own evidence.

## The live test matrix for CS_DCLF_BINDLESS_DRAW

Driven through `CS_DCLF_TEST_COMMANDS`, with record-dedup and BuildDraws parity on throughout.

| case | how | result |
| --- | --- | --- |
| interiors with rooms and portals | `coc WhiterunDragonsreach`, `coc WhiterunBanneredMare` | clean |
| exteriors | Whiterun, `coc Riverwood` | clean |
| cell transitions | six, across both runs | clean |
| worldspace change (fast-travel equivalent) | `coc Riverwood`, `coc Whiterun` | clean |
| night lighting / emissives | `set gamehour to 22` | clean |
| dungeon, alpha-tested foliage, god rays | `coc BleakFallsBarrow01` | clean |
| save | `save dclfm2` | clean |
| **load** | `load dclfm2` | clean — the path that frees renderer data |

Across both runs: **0 mismatches**, 21/21 BuildDraws parity OK, 20/20 record dedup parity OK, 0 rejected
buffers, and no VUIDs beyond the upstream semaphore one. Images correct in Dragonsreach, Bleak Falls
Barrow and Riverwood.

**Not covered, because they need input the console cannot drive:** kill cams, the map menu, first
person, Screenshot and Remote Control. Dynamic Cubemaps is not installed in this profile, so it was not
exercised either. The switch is therefore left **off by default** — the automatable part of the matrix is
clean, but the matrix is not complete, and that last step is a human one.
