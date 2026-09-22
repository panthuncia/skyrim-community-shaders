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
| `CS_DCLF_STATS=1` | Every 300 frames, log how many objects are tracked, why the rest stay native, and the CPU time scene capture takes, plus the GPU time of each render-graph segment and of the passes in it (`[ORG] GPU time from ORG's pass timestamps`; the menu shows the per-segment totals whatever the switch). |
| `CS_DCLF_CAPTURE_PARITY=1` | Compare the tables with the native draws (see above). Costs CPU on every draw. |
| `CS_DCLF_DEBUG_VIEW=1` | Before the deferred composite, copy DCLF's off-screen targets over the native ones: the frame shows only what the indirect draws produced. |
| `CS_DCLF_HYBRID=1` | DCLF draws into the main pass's own targets and depth, and the native loop skips the objects it drew. |
| `CS_DCLF_HYBRID_NOSKIP=1` | With the hybrid path, keep drawing everything natively, so what DCLF fails to draw is still visible. |
| `CS_DCLF_ONLY_ELIGIBLE=1` | The reverse skip: the native frame draws only the objects DCLF draws, so the two can be compared pixel for pixel. |
| `CS_DCLF_CULL=off\|frustum\|occlusion` | How BuildDrawsCS filters the candidates before writing their sequences: nothing, the frustum, or the frustum and then the HZB. |
| `CS_DCLF_CULL_INPUT=native\|tracked` | Which candidates may be drawn: only what the engine's culling kept (the default), or whatever the GPU culling keeps. `tracked` still shades incorrectly (see Phase 4). |
| `CS_DCLF_BUILD_PARITY=1` | Every 300 epochs, read BuildDraws' output back and compare it with the CPU templates (`BuildDraws parity OK/MISMATCH`). |
| `CS_DCLF=0` | Turns the feature off entirely: no hooks, no tables, no draws. Anything else, including unset, leaves it on. The feature list's on/off toggle, by contrast, applies live: off keeps the hooks and the scene tracking but does no frame work (nothing built, drawn, skipped or withheld, claims dropped), and back on resumes at the next frame without a rescan. Unloading the feature (`Feature::loaded`, the remote toggle) does the same. |
| `CS_DCLF_TABLES=tracked\|accumulated` | Whether the tables hold the whole tracked set or only what the engine's accumulator kept. `accumulated` was the fallback while the per-pipeline template defect was open; `tracked` is now correct. |
| `CS_DCLF_EVAL=off\|material\|geometry` | Diagnostic: suppresses parts of the stand-in evaluation. Note that a suppressed evaluation also stops its objects being drawn, so a clean frame under it proves nothing on its own. |
| `CS_DCLF_OWNERSHIP=static` | Withhold claimed passes from the main camera's batch renderer, so DCLF owns those objects outright. Default off. |
| `CS_DCLF_SHADOWS=1` | The shadow views: DCLF culls and draws the frame's casters into the engine's shadow map slices in one epoch per frame (below, "Shadow views, step two"). Default off. Live toggle in the menu. |
| `CS_DCLF_SHADOW_OWNERSHIP=static` | Withhold the casters DCLF's shadow epoch draws from the shadow views' batch renderers, per render mode. Needs `CS_DCLF_SHADOWS=1`. Default off. Live toggle. |
| `CS_DCLF_SHADOW_PROBE=1` | Diagnostic: the shadow probe (step one): per-view engine state, registrations, derivation and rule cross-checks, and the engine's shadow CPU. |
| `CS_DCLF_PASS_SOURCE=accumulator` | Build the tables from the accumulator walk instead of the captured registrations. |
| `CS_DCLF_MATERIAL_CACHE=probe` | Diagnostic: measures how many material records are unchanged from the previous frame, i.e. whether a cross-frame cache could work. |
| `CS_DCLF_EVAL=audit` | Diagnostic: snapshots pipeline state around every stand-in call and reports anything not restored. Very slow; the frame rate collapses. |
| `CS_DCLF_SHADER_DEBUG=1` | Build the Lighting and Utility SPIR-V with source-level debug info (`-Zi`: `OpSource` with every file's text embedded, and `OpLine`), so Nsight and RenderDoc show source for DCLF's draws. Still optimized. Not `-fspv-debug=vulkan`: its `DebugValue`s keep dead loads alive, so stages read resources their passes do not bind and every candidate is skipped; `vulkan-with-source` also fails DXC 1.9's own validator. There is deliberately no `-Od` form either: unoptimized code reads per-frame constant buffers the epochs do not supply (VS b6, PS b7). The Z-prepass stage (`DCLF_DEPTH_ONLY`) compiles the lighting out of `Lighting.hlsl` rather than relying on the optimizer. The debug builds have their own cache keys. The shader files the game sees through MO2's VFS are also copied, keeping their `Data/Shaders/...` layout, to `CS_DCLF_SHADER_SOURCE_DIR` (default `<Documents>\My Games\Skyrim Special Edition\SKSE\CommunityShaders-ShaderSource`). Shaders DXVK translates from DXBC get no source info this way. The build-time SPIR-V (BuildDrawsCS, HzbCS, LLF's cluster shaders) is always built with `-Zi` (`cmake/RenderGraph.cmake`). Dev-Fast builds do not package it: copy `build/Dev-Fast/generated/Shaders/*/ORG/*.spv` into the mod's `Shaders` folder after changing those shaders. |
| `CS_DCLF_TEST_TOGGLE=<off>:<on>` | Test runs: flips the feature's menu toggle off and back on at those frames (loading screens not counted), to exercise the live on/off. |
| `CS_DCLF_TEST_COMMANDS=<frame>:<command>;…` | Test runs: run each console command on the main thread once that many frames have been presented (loading screens do not count), for coverage runs from the auto-loaded save. The counter is independent of the feature, so a `CS_DCLF=0` control reaches the same place at the same hour. |
| `CS_DCLF_ASYNC=off\|on\|probe` | Where the epochs' payloads are built (see "Payloads built off the render thread"). `off` (default): inline, as before. `on`: the enabled jobs build on the `CS DCLF worker` thread and the epoch commits the result. `probe`: build on the worker *and* inline, and byte-compare the two payloads (`probe: N compared, N differ`). Bindless path only; the non-bindless path always builds inline. |
| `CS_DCLF_ASYNC_JOBS=colour,zprepass,shadow,scene` | Which jobs `on`/`probe` move to the worker (default: all four). For bisecting. |
| `CS_DCLF_ASYNC_WAIT_MS=<ms>` | How long an epoch waits for its job before building inline instead (default 3). |
| `CS_DCLF_ASYNC_PRIORITY=normal` | Run the worker at normal priority instead of above normal. |
| `CS_GPU_IDLE_TRACE=<frames>` / `CS_PROFILER_LOG=<frames>` | Not DCLF's, but the gates below read them: the GPU idle trace and its `[GpuIdle] summary` lines, and the profiler's averages in the log. See [render-graph.md](render-graph.md). |

### Capture tools and GPU timing

- Every render-graph pass is a `VK_EXT_debug_utils` region named after the pass (`cs.dclf.main-opaque`,
  `cs.dclf.build-draws`, ...), opened before the pass's entry barriers, so a wait on the previous pass
  shows up inside the pass that needed it. Each epoch's submission is also a queue label named after its
  segment (`CS DCLF: main opaque`, ...).
- DCLF's draws are device-generated commands (`vkCmdExecuteGeneratedCommandsEXT` over an indirect
  execution set), not `vkCmdDrawIndexedIndirect`. Nsight does not time the generated draws as a draw
  of their own, and bills their GPU time to the barrier recorded just before them. The pass timestamps
  are the reference instead. Measured on the auto-loaded save: main opaque 3.08 ms in the draw pass
  against 0.006 ms in BuildDraws' dispatches; Z-prepass 0.49 ms of draws and 0.10 ms of HZB; shadow
  views 1.29 ms of draws. The `vkCmdDrawIndexedIndirect` calls outside every DCLF region are DXVK's translation of
  Grass Optimizations' D3D11 `DrawIndexedInstancedIndirect` calls.
- The profiling window lists every render-graph pass that did work, from the same timestamps, under its
  feature: `DrawcallLimitFix::<segment> / <pass>`, and `LightLimitFix::RenderGraphCull / <pass>`. No D3D11
  timer brackets an epoch: it would straddle the epoch's submission and count the queue's idle time too
  (see "GPU timing in the profiling window" in [render-graph.md](render-graph.md)).
- RenderDoc (1.46) does not support `VK_EXT_descriptor_heap`. With RenderDoc capture on, the render graph
  cannot start and DCLF is forced off for the session; the log and the feature's menu page say why.

### Defaults

Unset switches now take the configuration every gate run of this work used: `CS_DCLF_HYBRID=1`,
`CS_DCLF_OWNERSHIP=static`, `CS_DCLF_CULL=occlusion`, `CS_DCLF_SKINNED=1`, `CS_DCLF_TREES=1`,
`CS_DCLF_DECALS=1`, `CS_DCLF_PROJECTED_UV=1`, `CS_DCLF_MTLAND=1`, `CS_DCLF_SHADOWS=1` and
`CS_DCLF_SHADOW_OWNERSHIP=static` (the tables already default to the tracked set). An explicit value
overrides a default: `0` for the class and path switches, `off` for the two ownership switches and the
culling. Rows above that say "Default off" describe the switches before this change. All of them are live
toggles in the menu.

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

## CS_DCLF_BINDLESS_DRAW defaults on

Validated over the live test matrix and enabled by default, together with `CS_DCLF_BINDLESS` that it
depends on. Both are disabled with `=0`, and `=0` is the control every pre-Step-D measurement in this
document was taken with.

With no switches set, Dragonsreach now builds **85 binding records** from ~1500 candidates and the epoch
costs ~1.02 ms against ~1.98 before. Gates on the default path: 11/11 BuildDraws parity, 11/11 record
dedup parity, 0 mismatches, no VUIDs beyond the upstream semaphore one. The map menu and the
first-person camera were checked by hand; the console-driven part of the matrix (interiors with rooms
and portals, exteriors, six cell transitions, a worldspace change, night lighting, a dungeon with
alpha-tested foliage, save and load) was clean.

## Bringing in a new object class: trees

### Which class, decided by histogram

`Ineligible::Technique` counted 847-1337 objects in the Whiterun exterior with no indication of what was
in it. Recording the rejecting technique gives:

| cell | rejected |
| --- | --- |
| Whiterun exterior | **treeAnim(12) = 1237**, MTLandLODBlend(19) = 96, MTLand(8) = 4 |
| Dragonsreach | refraction(63) = 5 |

One technique is 92% of the class. That is the one to bring in.

**The first version of this histogram read `none(0)=1337` and meant nothing.** `ClassifyStatic` copies
its descriptors to the caller only when the verdict is `None`, so on a rejection the caller kept a
default-initialised `rejectedTechnique` — and 0 is `none`, a *supported* technique. An empty field that
reads as a plausible value is the same failure as the material probe comparing padding and the validator
re-checking a fixed sample. The default is now 62, which cannot be mistaken for a real answer.

### What trees need, measured rather than guessed

`CS_DCLF_TREES=1` adds technique 12 to the supported set. Coverage in the Whiterun exterior goes from
~2900 to **3863 objects**, and `treeAnim` leaves the rejection histogram.

Capture parity then fails, identically with bindless on and off, so it is the trees and not the newly
defaulted switch. The sample list could not say why — it is capped, and the standing `PS PerMaterial 29`
difference filled it, so 83,396 per-geometry mismatches produced not one sample. Counting mismatches
**per variable** instead names them at once:

| variable | mismatches |
| --- | --- |
| **VS PerGeometry 5 — WindTimers** | 83,390 |
| **VS PerGeometry 4 — TreeParams** | 47,851 |
| PS PerMaterial 29 — IBLParams (standing) | 57,300 |

Declaration order in `Lighting.hlsl`'s VS PerGeometry block is World, PreviousWorld, EyePosition,
LandBlendParams, **TreeParams**, **WindTimers**, so 4 and 5 are exactly the two constants tree animation
displaces its vertices with.

This is the defect the switch was put there to catch, and it is the same shape as the culled lighting
template: **DCLF's PerGeometry constants are per PIPELINE**, taken from one template object, while
TreeParams is a property of the individual tree and WindTimers advances during the frame. One template
tree's wind is handed to every other tree sharing its pipeline.

### What finishing it requires

TreeParams and WindTimers have to become per-object values carried in the object record, exactly as Step
C did for World, PreviousWorld and MaterialData: add them to `BindlessObject`, read them per object in
`BuildFrame`, and take them from the record in the shader under `DCLF_BINDLESS_DRAW` (and through
`PatchObjectGeometry` otherwise). Where the engine computes TreeParams for a given tree still has to be
established — `BSLightingShader::SetupGeometry` is the place to read it from, the same way IBLParams was
traced to the shader object. `CS_DCLF_TREES` stays off until that lands.

### Where the engine gets TreeParams and WindTimers

`BSLightingShader::SetupGeometry` is vtable slot 6 — `BSLightingShader::Func6` at `1414dd040`. Its
`case 0xc` is technique 12, TreeAnim. The VS constant table base is `0x50` (the PS one is `0x40`, which
`SetupMaterial`'s `0x5d` for variable 29 confirmed), so `0x54` is variable 4, TreeParams, and `0x55` is
variable 5, WindTimers.

The data comes from a **`BSTreeNode`**, reached from the shader property:

```
property->fadeNode                     // BSShaderProperty + 0x60
    ->vfunc[0x1f8 / 8 = 63]()          // downcast; null for anything that is not a tree
```

`BSTreeNode` is a real engine class — it registers by that name alongside `BSFadeNode` in the class
table at `FUN_14147e2c0`. Its wind state:

| offset | meaning |
| --- | --- |
| `+0x158` | squared distance (the code takes its square root with the `0x5f3759df` fast inverse square root and one Newton step) |
| `+0x15c` | maximum amplitude |
| `+0x160` | leaf frequency |
| `+0x164` | wind timer |
| `+0x168` | previous wind timer |

and the derivation, with `node` null giving `distance = 0` and `maxAmplitude = 1`:

```
TreeParams.x = 0
TreeParams.y = *(0x142033060 + 0x304)                       // global wind magnitude
TreeParams.z = clamp((1 - (sqrt(node[0x158]) - A) / (B - A)) * maxAmplitude, 0, maxAmplitude)
TreeParams.w = node ? node[0x160] : 1
WindTimers.x = node ? node[0x164] * K : 0
WindTimers.y = node ? node[0x168] * K : 0
```

`A` = `DAT_142033100` and `B` = `DAT_142033104` — a distance fade, written by one function and read by
`BSUtilityShader::SetupGeometry` as well. `K` = `DAT_141ad28bc`, a timer scale.

**SetupGeometry mutates the node.** Its last act in this case is

```c
*(undefined4 *)(lVar22 + 0x168) = *(undefined4 *)(lVar22 + 0x164);   // previousWindTimer = windTimer
```

so the engine advances each tree's animation state as a side effect of setting up its draw. Two
consequences. A per-object *stand-in* evaluation — calling the engine's SetupGeometry once per tree the
way `EvaluateMaterial` does for materials — would advance every tree twice a frame and is therefore not
an option here. And whenever DCLF does own the tree pass outright, it has to perform that write itself,
exactly once, or tree motion stops.

The remaining work is bounded: carry TreeParams and WindTimers per object (a parallel array plus two
more `GeometryPatchOffsets` entries is enough for the non-bindless path, and needs no shader change),
derive them as above, and let capture parity's new per-variable histogram confirm it — a wrong constant
shows up immediately as `VS PerGeometry 4` or `5`, which is what makes replicating engine arithmetic
safe to attempt at all. Only then do the two values need adding to `BindlessObject` and the shader.

### Trees, landed

`CS_DCLF_TREES=1` brings technique 12 into coverage. The Whiterun exterior goes from ~2900 to **3862
objects**, and `treeAnim` leaves the rejection histogram entirely.

TreeParams and WindTimers are carried per object: a `tables.treeAnim` array parallel to `objects`, two
more `GeometryPatchOffsets` entries for the constant-buffer path, and an `ObjectTreeAnim` at the end of
`BindlessObject` (144 -> 176 bytes) with matching `float4`s in the GPU record, since under bindless the
PerGeometry block is one pair for the whole pipeline. `windTimers` is a `float4` using only `xy` so the
two layouts cannot disagree about padding.

Two defects on the way, both caught by the per-variable histogram within one run each.

**The first version wrote zeroes over every object in the frame.** `tables.treeAnim` is filled for all
objects and `PatchObjectGeometry` wrote it unconditionally, so every *non*-tree lost the TreeParams and
WindTimers its pipeline template had supplied. Parity went from 47,851/83,390 mismatches to
126,942/124,505 — worse, and the samples read `DCLF 0, native 1`, which is what a clobber looks like.
Writing them only under a new `kObjectTreeAnim` object flag took WindTimers to **0** and TreeParams to
**600**.

**The last 600 were the square root.** The engine does not call `sqrtf`: it puts the squared distance
through the `0x5f3759df` fast inverse square root with one Newton step and multiplies back by x.
Substituting `std::sqrt` agreed for every tree that clamps at one end of the fade band and disagreed for
exactly the population inside it — 600 draws a frame, 0.7%. Reproducing the approximation bit for bit
closes it.

**Gates.** Constant-buffer path: capture parity **0 per-geometry mismatches** across four intervals,
against a no-trees control on the same build with identical material numbers (57,300 + ~3,000, both
pre-existing). Bindless path: **443M bindless record components matching**, 20/20 record dedup parity,
21/21 BuildDraws parity, 0 mismatches, no new VUIDs, and trees render correctly.

`CS_DCLF_TREES` is left **off by default**: the gates are clean, but it is a large coverage change and
the project's practice is that a switch defaults on only after someone has looked at it moving.

### A correction to earlier measurements in this document

Several "capture parity 0 mismatched" readings above were taken from the *tail* of a run's log, which is
always the cell the run `coc`s into — Dragonsreach. The Whiterun exterior intervals of those same runs
were not clean: 22,228 material mismatches, all `PS PerMaterial 29` (IBLParams).

This is not caused by the material cache, and the cache is not a regression — with the cache **off** the
same exterior reads **297,589**, five times worse, because nothing then resamples IBLParams at Prepass.
The residue is the sub-frame lag already described: outdoors the value moves far more within a frame
than it does inside Dragonsreach, and one sample at Prepass no longer covers it. `VS PerMaterial 11`
(TexcoordOffset, animated per material) contributes a further ~3,000 and is genuinely uncacheable.

The lesson is the same one the instruments keep teaching: **read every interval, not the tail.** A run
that visits two cells reports two different answers.

## BSEffectShader: investigated, and not worth doing

`not-lighting-shader` is the largest rejection class by tracked count — 3057 in the Whiterun exterior
after trees — so it looked like the obvious next coverage target, and the assumption was that it meant
repeating Steps A-D for a second shader. Three measurements say otherwise, and together they redirect
the work.

**What the class actually is.** Recording the rejected property's RTTI rather than just counting:

| cell | property types |
| --- | --- |
| Whiterun exterior | BSEffectShaderProperty 2701, *no property at all* 343, BSWaterShaderProperty 13 |
| Dragonsreach | BSEffectShaderProperty 235, *no property* 1 |

343 of them have no shader property, so nothing could ever draw them.

**Over half is alpha blended.** 1717 blended against 1340 opaque outdoors, and 208 against 28 inside.
Blended geometry is not drawn in the pass DCLF owns: it goes after the deferred composite, forward and
sorted back to front, while the epoch writes the G-buffer with depth test EQUAL before the composite.
Covering it would mean a **new epoch**, not another shader in the existing one.

**And almost none of it is drawn in the main pass anyway.** The `RegisterPass` probe, counted per report
interval (~300 frames) and split by whether the batch renderer belongs to the main camera:

| shader type | into a main renderer / total | per frame |
| --- | --- | --- |
| **Lighting (6)** | 603,974 / 618,674 | **~2013** |
| **Effect (7)** | 3,900 / 3,900 | **~13** |
| Grass (1) | 3,300 / 6,600 | ~11 |
| Water (3) | 11,700 / 11,700 | ~39 |
| Utility (8) | 0 / 1,537,933 | 0 |

**BSEffectShader registers about thirteen passes a frame.** The 2701 tracked effect objects are
overwhelmingly magic and environment effects that are not currently drawing. A second shader pipeline —
its own stand-in evaluation, constant layout, permutation compilation, pass-capture filter and record
shape — would buy thirteen draws.

### Where the remaining coverage actually is

DCLF claims 1464 of the ~2013 Lighting passes a frame. **The gap is ~549 draws a frame, and all of it is
inside the Lighting shader**, where every piece of machinery already exists. Tracked counts for what is
still rejected in the Whiterun exterior, trees on:

| reason | tracked | what it needs |
| --- | --- | --- |
| projected-uv | 860 | the ProjectedUV constants, per object |
| skinned | 633 | the bone palette in the record |
| unsupported-parent | 630 | scene-graph shapes the tracker declines to follow |
| decal | 601 | blending and depth handling |
| not-trishape | 274 | dynamic tri shapes |
| technique (MTLand, MTLandLODBlend) | 100 | terrain, the per-object constant question trees had |

Trees were worth doing because 1237 tracked objects turned into real draws. These are the classes to
rank next, by how many of their tracked objects the engine actually draws — a number the registration
probe can give per class, and the one that should decide the order rather than the tracked count that
made BSEffectShader look like the prize.

## Ranking the remaining coverage by what the engine actually draws

Tracked count is the wrong measure, and ranking by it is what made BSEffectShader look like the prize.
The right one is: of the objects DCLF rejects, how many did the engine itself register a main-pass
lighting draw for this frame? That is `accumulated != nullptr` at the point of rejection, so it costs a
counter and no new machinery.

Whiterun exterior, trees on, per frame:

| reason | tracked | **drawn** | of tracked |
| --- | --- | --- | --- |
| **decal** | 601 | **181** | 30% |
| **skinned** | 633 | **150** | 24% |
| projected-uv | 860 | 53 | 6% |
| not-trishape | 274 | 44 | 16% |
| technique (MTLand, MTLandLODBlend) | 100 | 33 | 33% |
| actor | 27 | 21 | 78% |
| unsupported-parent | 630 | 7 | 1% |
| **not-lighting-shader** | **3057** | **0** | **0%** |

Dragonsreach agrees: decal 88 -> 35, skinned 52 -> 30, not-trishape 52 -> 26, not-lighting-shader
51 -> **0**.

The order is almost the reverse of the tracked counts. `projected-uv` led on tracked objects (860) and
is sixth on drawn ones; `unsupported-parent` has 630 tracked and draws seven. And
**not-lighting-shader draws nothing at all** — stronger than the ~13 a frame the registration probe
suggested, and a complete answer on BSEffectShader.

Everything still rejected totals ~490 drawn a frame, against the 1464 DCLF claims.

### Feasibility, which does not follow the same order

DCLF's pipeline expresses exactly one piece of fixed-function state, `kRasterTwoSided`; depth test EQUAL
with no write, and blending and depth bias off, are assumptions baked into every pipeline it builds. The
VS layout gives Bones no real storage. So:

-   **decal (181)** needs depth bias and blending in the `PipelineKey`, and an ordering guarantee
    against the base geometry underneath. Structural.
-   **skinned (150)** needs a bone palette per object — a variable-length array, unlike every per-object
    value the record carries today. Structural.
-   **projected-uv (53)** needs the ProjectedUV constants, and `TextureProj` is **already** VS
    PerGeometry variable 6 in the layout. Plausibly the trees pattern again: derive it per object, patch
    it, gate with capture parity.
-   **technique MTLand / MTLandLODBlend (33)** is terrain, and wants per-object `LandBlendParams` —
    VS PerGeometry variable 3, also already in the layout. The same pattern once more.

So the two largest classes are the two that need new architecture, and the two that fit the existing
machinery are worth ~86 draws a frame between them. That is the trade to decide before any of it is
built, and it is not what either the tracked counts or the drawn counts say on their own.

## Decals: a second draw pass, and a single culling phase

The ranking put decals first at 181 drawn a frame in the Whiterun exterior, and the feasibility read said
they needed depth bias and blending in the pipeline key and an ordering guarantee against the geometry
underneath. Both turned out to be true and neither turned out to be large, because the engine's own
decal path answers the question that decides the architecture: **can a decal change the silhouette of
what it sits on?**

It cannot, and `docs/development/skyrim-engine-notes.md` ("Decals") has the decompile that says so. The
main pass draws its two decal groups after every opaque object, under a depth test against them, and
the opaque group's depth write is the host's depth pulled a bias toward the camera. A decal never
occludes anything its host does not. So decals are **occludees only**: they never enter the Z-prepass or
the two-phase replay, they are culled **once**, in the colour segment, against the HZB that the depth
segment rebuilt from this frame's depth, and they are drawn by a **second pass** in the same segment,
after the opaque colour pass, that tests LESS_EQUAL with the engine's bias and writes no depth at all.

### What the probe found before anything was built

`CS_DCLF_DECAL_PROBE=1` records, at every native decal draw, the accumulation hint, the alpha property,
the depth flags and the `RendererShadowState` indices the draw was issued with, and reads the D3D11
description of every distinct state object once. It corrected the decompile-derived plan in one place -
the group index is the hint plus one - and settled two things the decompile could not: which render flags
each group runs with (0x41 for hint 2, so the alpha property is not applied; 0x45 for hint 3, so it is),
and that both blend states the groups use fit BasicRHI's `BlendFactor` without an RHI change.

Whiterun exterior, per frame: 199 native decal draws, 140 in the opaque group and 58 in the blended one;
Dragonsreach 30 (17 / 13). The `RenderPassImmediately` thunks see every one of them (211 offered against
199 drawn), which is the hook-coverage question answered: withholding a decal's pass stops its native
draw.

### The pipeline key carries the engine's state indices

`PipelineKey::rasterFlags` was one bit (two-sided). It now packs the decal group and the engine's own
`rasterStateDepthBiasMode`, `alphaBlendMode`, `alphaBlendAlphaToCoverage`, `alphaBlendWriteMode` and
`alphaBlendModeExtra` (Records.h). Every opaque key keeps all of those at zero, so nothing about an
existing pipeline changed - the pipeline count did not move.

The indices are translated by reading the engine's state objects, not by re-deriving what each index
means: `DrawPipelines::CaptureEngineStates` takes `GetDesc` of the rasterizer and blend states behind
each key's bits (EngineStates.h: the tables at `RelocationID(524748, 411363)` and `(524749, 411364)`) and
keeps them in RHI terms for the asynchronous build. It runs from the first lighting draw of the deferred
pass, deliberately: Community Shaders swaps the blend table for its deferred variants between
`StartDeferred` and `ResetBlendStates`, and those - with RT1-2 forced to alpha blend - are what a native
decal in the G-buffer uses. A key whose state has not been captured is simply not ready.

Where the indices come from once DCLF owns the decal and there is no native draw to copy: the bias mode
from `DecalDepthBiasMode` (the `ToggleDepthBias` byte and `DrawWorld::disableSunShadows`, so interiors get
the no-bias modes 7 and 11), the blend mode from the alpha property's functions as `FUN_14150bc80`
chooses it, the write mode as the group function and SetupGeometry leave it (10; 1 with `kZBufferWrite`,
else 11). The probe checks the derivation against every native decal draw it still sees: **0 of 4,800
mismatched in Dragonsreach, 300 of 52,800 in the exterior**, every one of them write mode 11 derived
against 1 observed on a decal without `kZBufferWrite`. That is a leak from Community Shaders' Terrain
Blending, which sets write mode 1 for its own passes from the same hook that precedes the blended decals;
the only difference between the two modes is RT0's alpha mask, and the deferred composite never reads it.

### Ordering, by fixed slots rather than by an atomic append

Overlapping decals are drawn in the engine's order - the opaque group, then the blended one; within a
group the technique buckets ascending, each bucket's five lists, each list's chain - and RegisterPass
prepends, so a chain is drawn in reverse registration order. An atomic append in BuildDraws cannot promise
that order, and two overlapping decals landing in a different order each frame would flicker.

So a decal's sequence goes to a **fixed slot**: SceneStore sorts the frame's decals by that key
(`Tables::decalOrdinal`), the epoch hands each its ordinal in its `DrawInput`, and BuildDrawsCS writes
`sequences[kDecalSequenceBase + group * kMaxDecalDraws + ordinal]` - either the draw, or the same
sequence with an index count of zero when the culling rejected it or the epoch could not build its
record. Every decal input writes its slot, culled or not, so nothing a previous frame left there can be
executed; a guard in the epoch loop pushes a blank input on every path that gives up on a decal. Each
group's draw reads its slot count from its own count word, which the CPU uploads.

### The second pass

`MainOpaquePass::Record` begins a second pass on the same attachments, all loaded, after the opaque
`ExecuteIndirect`, and issues one indirect draw per group from that group's range. The depth segment
never sees a decal: its epoch skips them before the cull-only push, so they get no visibility verdict and
none is needed - the colour segment's BuildDraws tests them itself, frustum and HZB, and the HZB bound
there is the one rebuilt at the end of this frame's depth segment, so it is final. A decal with
`kZBufferWrite` keeps its native depth-pass draw (`SkipNativePass` exempts decals inside the depth pass):
DCLF writes no decal depth, and a depth-writing decal withheld from that pass would lose its depth
entirely.

Two instrument corrections came out of the parity runs. The per-geometry check compared TreeParams and
WindTimers for every object, and they compared clean by accident: SetupGeometry writes them for technique
12 only, the native buffer keeps whatever the last tree left, and no eligible object had been drawn after
a tree in the main range - until decals, which the engine draws after everything. The check now ignores
those two variables for non-trees. And the capped sample list is now two samples per variable, because
the standing PS PerMaterial 29 difference had filled it on its own for three stages running.

### Decals, landed (behind `CS_DCLF_DECALS`)

Whiterun exterior on the hybrid path (`CS_DCLF_OWNERSHIP=static`, `CS_DCLF_CULL=occlusion`,
`CS_DCLF_TABLES=tracked`), per frame: **176 decals submitted to the second pass** (136 in the opaque
group, 40 in the blended one), 28 of them culled; `decal ... 0 drawn` left native, down from 181. Claims
1465 -> 1641 with **0 claimed but not drawn**, decal build parity **176 slots, 0 differ**, opaque build
parity unchanged, 0 validation messages. Dragonsreach: 16 submitted, 3 culled, 0 differ.

Cost, profiled, against a control with the switch off at the same place: the table build 4.52 against
4.51 ms (noise), the colour epoch 2.06 against 1.90 ms - 0.16 ms for 176 more draws, most of it the graph
execution (+0.06-0.09) and the epoch prologue (+0.03). The native loop gives up the 176 draws.

**And the image got better, not merely equal.** The control frame is missing the cobblestone road to the
right of the player; the decal frame has it. The road is a blended decal (hint 3), which the engine draws
with depth test only, before `EndDeferred`; DCLF's opaque colour pass then ran at `BeforeDeferredComposite`,
after it, and painted the ground back over it - a standing defect of the hybrid path that nothing had
measured, because capture parity compares constants and the decal's constants were fine. Owning the decal
and drawing it after DCLF's own opaque pass is what restores the order the engine has. Opaque decals
(hint 2) survived before because their depth write wins the host's LESS_EQUAL test.

Left off by default, as trees are, until someone has watched decals in motion (blood, overlapping placed
decals, cell transitions). The switch set that validated it is the one above plus `CS_DCLF_DECALS=1`;
`CS_DCLF_DECAL_PROBE=1` reports the state parity and stays useful whenever ownership is off.

## Skinned objects: the engine's palette, DCLF's buffer

Skinned objects were the second-largest class by drawn count (150 a frame in the exterior, ~100 in
Dragonsreach) and the one the feasibility read called structural, because a bone palette is a
variable-length per-object array and nothing in the record was. The structural part turned out to be
small, because the engine already does the hard half: `docs/development/skyrim-engine-notes.md`
("Skinning") has the decompile. `NiSkinInstance::boneMatrices` holds the whole palette as three float4
rows a bone, in absolute world space, refreshed once a frame by `FUN_140e4ff90` under a frame stamp, with
the previous frame's palette kept beside it. The bone setter that runs from the native draw copies those
rows into b10 and b9 and nothing more. So DCLF has no skeleton, no bind pose and no palette builder. It
runs the same update from `BuildFrame` (the native draw it withholds would otherwise have been the only
caller), copies the rows into `Tables::bones` / `previousBones`, and gives each object an offset.

### What went where

-   **The buffers.** A skinned draw binds `partition->buffData`, the skin partition's own `TriShape`,
    not `geometry->rendererData` (measured: never the same object). The geometry entry for a skinned
    shape is built from the partition, with `partition->vertices` and `partition->triangles`, and it
    resolves through `GpuResources` like any other TriShape. Draw parity confirms the binding on every
    native draw.
-   **The palette buffer.** `DCLFBones`, a `StructuredBuffer<float4>` at VS t126 beside the object
    table, sized 65,536 rows; the pipeline layout's vertex-stage SRV range is now two registers. The
    epoch packs the frame's rows **eye-relative** the way it packs World - each row's translation loses
    the eye's component for that row - current rows first, then the previous frame's relative to the
    previous eye. The shader's pivot is therefore zero, and the depth epoch, which has its own eye, is
    right by construction; `BonesPivot` in the mirrored per-frame buffer (measured equal to `posAdjust`
    on all 62,000 draws checked) is not read at all.
-   **The record.** `BindlessObject` grew a row: `boneOffset`, `previousBoneOffset`, `boneRows`
    (176 -> 192 bytes). Under `DCLF_BINDLESS` the SKINNED vertex path calls
    `Skinned::GetBoneTransformMatrixBindless` / `GetBoneRSMatrixBindless` on `DCLFBones` at the object's
    offsets and declares no b9/b10 at all. The record deduplication is untouched: nothing per object
    entered the binding record.
-   **Eligibility** (`CS_DCLF_SKINNED=1`): a `BSTriShape` with an accumulated pass whose skin instance is
    exactly a `NiSkinInstance` with one partition and at most 80 bones (`skin-shape` counts the rest); the
    `kSkinned` property flag then selects the SKINNED permutation as before.

### Measured

Gate 1, ownership off, capture parity on. **Bone palette parity: 0 of 7,800 palettes differ in the
exterior, 0 of 42,000 in Dragonsreach** - the rows DCLF copies are byte-identical to the ones the native
draw bound at b9 and b10. Draw parity 0 differ, bindless record parity 0 differ over the new row, no new
material mismatches. The one new per-geometry class was World / PreviousWorld on skinned draws, which
SetupGeometry does not write for a skinned pass and the SKINNED vertex shader does not read; the parity
instrument now skips them for skinned objects, as it skips the wind variables for non-trees.

Gate 2, the hybrid path with static ownership and occlusion culling:

| | Dragonsreach | Whiterun exterior |
| --- | --- | --- |
| skinned candidates a frame | 250 | 33 |
| claims (control -> skinned) | 920 -> 990 | 1465 -> 1478 |
| claimed but not drawn | 0 | 0 |
| build parity | OK | OK |
| frustum false negatives | 0 | 0 |
| `skinning` build part | 0.071 ms | 0.009 ms |
| tables, profiled | 2.26 vs 1.83 ms | 4.83 ms |
| colour epoch | 1.36 vs 1.21 ms | 2.20 ms |

The A/B screenshots in Dragonsreach are the same frame; the banners are DCLF's in one and native in the
other. `CS_DCLF_SKINNED` stays off by default until someone has watched skinned objects in motion.

### What the probe put next

The exterior's single-partition class is small (33) because most of its skinned draws are elsewhere:
`skin-shape` leaves 94 a frame native there, 24 in Dragonsreach. The biggest piece is the
three-partition, tree-animated shape (29 a frame, hint 11, technique 12) whose partitions carry LOD bytes
1/2/0: the engine draws the partition the pass's LOD mode enables, through the table at `0x14202a030`,
so covering it means one DCLF object per enabled partition and reading that table. Then
`BSDismemberSkinInstance` (actor parts, one or two draws each, partitions toggled per frame). The
static-pose skip the plan reserved is not worth building: 191 of 201 palettes in the exterior change
every frame.

## ProjectedUV and terrain: the trees pattern, twice

Both classes were the "trees pattern": one per-object PerGeometry value the engine computes in
`SetupGeometry`, derived on the CPU and carried per object. They turned out to be that plus two things
the ranking could not see: ProjectedUV has three per-object *pixel* constants and four pipeline-level
textures as well as its matrix, and terrain is owned by Terrain Blending in this profile.

The per-object values live in the epoch's row buffer, after the bone palettes: `kExtraRows` (seven float4
rows) per object that needs them - LandBlendParams, the three rows of TextureProj, then ProjectedUVParams,
2 and 3 - with `BindlessObject::extraOffset` pointing at them and `DCLFObjects.hlsli`'s statics reading
them under `DCLF_BINDLESS`. The constant-buffer path patches the same variables through
`PatchObjectGeometry`, which is what capture parity compares. `SceneStore::RefreshObjectExtras` fills the
rows at Prepass, where the main camera's `posAdjust` is current, and it calls the engine's own
NiTransform-to-matrix and `D3DXMatrixMultiply` routines so that `TextureProj` is the native value to the
bit. The derivations themselves are in `docs/development/skyrim-engine-notes.md` ("ProjectedUV and
MTLand").

### What the gates found

`CS_DCLF_PROJECTED_UV=1 CS_DCLF_MTLAND=1`, ownership off, capture parity on, Whiterun exterior: 53
projected and 100 terrain candidates a frame; **no mismatches on LandBlendParams, TextureProj or the three
ProjectedUVParams** - the derivations are exact - and `projected-uv` and `technique` gone from the
left-native list. Two other things surfaced and were fixed:

-   **Terrain drew with the wrong vertex stride.** Draw parity reported every terrain draw with stride
    32 against the native 40. CommonLib's `VertexDesc::GetSize` sums the attribute sizes it knows and
    leaves the landscape data out; the engine binds the desc's stride nibble times four, which the engine
    notes had recorded all along. The geometry record now takes the nibble. It never showed before because
    no eligible object had landscape data.
-   **One tree a frame with amplitude 0 against 9.** Shrubs and ferns that are both tree-animated and
    projected became eligible, and one of them (`L2_SwordFern03`) holds an uninitialised negative squared
    distance on its node. The engine's fast square root shifts the bit pattern arithmetically and its
    result clamps to the maximum amplitude; DCLF's shifted logically and clamped to zero. A diagnostic
    that prints the derivation's inputs beside the live node's found it in one run; `FastSqrt` now
    matches the engine's shift.

### Terrain and Terrain Blending

Terrain Blending, on by default here, intercepts every `kMultiTextureLandscape` pass at
`RenderPassImmediately`, holds it, and redraws it after the opaque pass with alpha blending and its own
depth-stencil state (drawing below the surface, against a separate terrain depth). An opaque DCLF draw of
the same terrain would run under that, so `MtLandEnabled()` is false whenever Terrain Blending is loaded
and enabled *and* DCLF is drawing into the frame (the hybrid path). The tables and the parity still
exercise the derivation with the feature on, because its redraws happen inside the deferred pass where the
parity hook sees them; ownership of terrain needs either the feature off or a terrain sub-pass that does
what it does. The hybrid run with every class on reports 0 terrain candidates, as intended.

### Everything on

Hybrid, `CS_DCLF_TREES CS_DCLF_DECALS CS_DCLF_SKINNED CS_DCLF_PROJECTED_UV CS_DCLF_MTLAND`, Whiterun
exterior: claims 1478 -> **1712**, 0 claimed but not drawn, build parity OK, 0 frustum false negatives,
one validation message (the semaphore one). Left native and drawn: skin-shape 89, unsupported-parent 51,
not-trishape 48, technique 35 (terrain, gated), actor 35, alpha-test-state 1.

## Persistent tables: the build that stops re-deriving the frame

`BuildFrame` used to rebuild every table from scratch each frame. In the Whiterun exterior that was 4.3 ms
for 10,000 tracked objects of which ~1,700 are drawn, and profiled by part it was almost all re-derivation
of things that do not change: the static classification (1.2 ms), the per-frame hidden/fade check for
every candidate (0.7), the record (0.95) and the pipeline/material dedup (0.5). Two steps, both behind
gates that read 0, took it to **2.1 ms**.

### A1: nothing for what cannot be drawn

With the draws gated on the engine's visibility (`CS_DCLF_CULL_INPUT=native`, the default) an object the
accumulator does not hold is a culling candidate and nothing more: it needs bounds, `kObjectNoBindings`
and a place in every parallel array. It now gets exactly that, and only that, and the depth segment still
submits it cull-only, so the culling's cross-tabulation is unchanged (5,577 tested before and after).
`=tracked` keeps the full path because it wants records for them.

Whether such an object is a candidate at all is the classification's answer, and that answer is kept on
the tracked entry (`Tracked::candidateReason`) and refreshed every 64 frames rather than recomputed per
frame: once the derived cache below had removed the classification itself, the loop head - the witness
loads and the negative-cache probe for ~8,300 objects - was most of what "classify-static" still measured.
A verdict a few frames old costs at most a diagnostic, because a candidate is tested by the culling and
drawn by nothing. The ineligibility histogram counts the kept verdicts, so it reads as before.

### A2: slot-stable tables and the derived cache

The three shared tables - geometries, pipelines, materials - keep their slots across frames. Each slot has
a `lastUsed` frame and its key; a sweep every 16 frames frees slots idle for 64 and erases their map
entries; the per-object arrays are still appended per frame (`Tables::ClearFrame`). Everything that reads
a pipeline by index (the epoch's blocks, EarlyPrepass's pipeline requests, `RefreshFrameConstants`) is
restricted to the slots used this frame.

Over that, `Tracked::Derived` caches the positive derivation for an accumulated object: the descriptors,
the static object flags, the pipeline key and the three slots. Its witnesses are the Stage 3 pointers
(TriShape, property, material) plus the fade state, the accumulated technique / sub-pass / hint, the
interior flag, whether the material alpha is below one, the frame's decal bias modes and the table
generation; and the slots are checked against their keys before they are served, so a swept and reused
slot cannot be handed back. A hit skips `ClassifyStatic` and the whole derived section; `ClassifyFrame`,
the alpha-test-state rule, the transforms, shading, palettes and the draw sequence still run.
`CS_DCLF_DERIVED_CACHE=off|on|probe` (`probe` serves the cache and recomputes, comparing slots, flags, key
and descriptors): **0 differ** over 1,712 objects a frame in the exterior, through Dragonsreach, back,
and a save/load, with the sweeps retiring 1,000-1,600 slots at each transition and 0 slot violations.

Three things the persistence surfaced, each of which was a one-frame artefact before and a standing one
after, which is the general lesson of this step - every value in a persistent slot has to be either
fixed or refreshed on purpose:

-   **The technique constants are the frame's** (fog, settings, and the shadow mask's view). They are now
    re-evaluated at Prepass for the used pipelines, with the geometry templates. Serving the slot's first
    evaluation was a parity regression and, at startup, a stale view pointer.
-   **The per-frame drift floats** (PS PerMaterial 29) were learned by re-evaluating a cached material,
    which a persistent slot never does. The rolling validation slice (8 live materials a frame,
    re-evaluated and compared with what their slot serves) now learns them, seeds the patch source, and
    heals a stale record in place; at startup it reports a handful of stale slots as values settle, then
    0. Capture parity with every class on: PerMaterial 29 x91,200 and VS PerMaterial 11 x5,860 over
    523,000 checked draws, against x67,200 / x3,000 over 465,000 before - the same residue (standing debt),
    at a slightly higher rate that belongs with it.
-   **`GpuResources::Resolve` returned a pointer into a map whose storage moves.** Resolving the index
    buffer could reallocate the map and the vertex buffer's pointer then read garbage; rebuilt every frame
    the bad record lasted one frame, kept in a slot it lost the device on the first draw (3 of 14 startups,
    always at the first epoch). `CS_DCLF_SLOT_PROBE=1` - a fresh evaluation and a fresh resolve for every
    used slot, logged against what the slot serves - caught it in one failing run
    (`vb 0xb2104741d736cab1 -> 0x2562278000`, index address correct). `Resolve` returns by value now;
    0 of 6 startups after, 0 probe differences.

Also kept from this: a geometry slot found by address whose buffers no longer match (a TriShape
reallocated at the same address), or whose buffer references were evicted, or which was resolved while
the graph was off, is resolved again in place; the buffer references are touched on the slot's first use
of a frame; a pipeline's template is always a property of an object of the frame; `CheckObjectSlots`
neutralises any object naming a slot not of this frame and counts it (the gate: 0); the teardown paths
reset the maps and bump the generation.

### Measured, and the A3 decision

Whiterun exterior, hybrid, every class on, `CS_DCLF_PROFILE=1`, same place and hour:

| tables, ms      | control | A1   | A1+A2 |
|-----------------|---------|------|-------|
| total           | 4.8     | 4.3  | 2.12  |
| classify-static | 1.29    | 1.15 | 0.21  |
| classify-frame  | 0.68    | 0.69 | 0.19  |
| record          | 0.95    | 0.78 | 0.67  |
| dedup-hit       | 0.53    | 0.18 | 0.10  |
| loop-tail       | 0.38    | 0.36 | 0.36  |
| walk + lookup   | 0.47    | 0.48 | 0.46  |

Claims 1712 -> 1712, 0 claimed but not drawn, build parity OK, 5,578 tested / 0 frustum false negatives,
bone palette parity 0 of 7,800. What remains is per frame by nature: the walk and pass lookup (0.46), the
iteration over 10,000 tracked entries (0.36), and for the drawn objects their transforms, bounds, room
probe, shading and sequence (0.67 for 1,712 plus the 3,225 cull-only pushes). A3 - persistent object
slots - would save the pushes and the `objectIndex` insert, on the order of 0.2-0.3 ms of the 2.1, and is
not built; the next structural gain is Stage 6, where the engine's own per-object main-pass work is.

## Shadow views, step one: what the engine does, and a frame in two halves

### The probe (`CS_DCLF_SHADOW_PROBE=1`)

`DrawcallLimitFix/ShadowProbe.cpp` measures the shadow half of the frame before DCLF owns any of it: it
enumerates the views from the shadow scene node before the shadow maps are drawn, records the render
state at each shadow accumulator's `FinishAccumulatingPreResolveDepth`, samples the Utility draws'
constants, times each light, and cross-checks every Utility registration against the technique DCLF
would derive and the caster rule DCLF would apply. What it found is written up in
`skyrim-engine-notes.md` under "Shadow maps"; the four answers the design needed:

-   **The derivation is exact.** Over ~1M registrations in the exterior and the Bannered Mare, the
    technique derived from the property's flags and the view's render mode differed from the engine's
    `passEnum - 0x2B` **zero** times. For a Utility pass the `techniqueID` given to `RegisterPass` is the
    `passEnum` itself, not a descriptor as it is for Lighting.
-   **The caster rule is exact.** The rejection rule read off `GetRenderPasses_ShadowMapOrMask` rejected
    none of the casters the engine registered. Of the main pass's kept objects with no registration in a
    cascade, all but the alpha-blended ones are simply outside the cascade cull.
-   **Views need a viewport origin.** A paraboloid light's two hemispheres share one array slice as
    `(0,0) 4096x2048` and `(0,2048) 4096x2048`, and a focus shadow is `(0,3547) 549x549`. Half the drawn
    views are sub-rectangles, so `rhi::PassBeginInfo` cannot keep assuming the origin is (0,0).
-   **The eye is the shadow camera's.** Utility's `World` is eye-relative to the shadow camera's
    `posAdjust`, up to 15,000 units from the main camera's, and `VS_PerFrame` c8 holds the transpose of
    that camera's `viewProjMat`. A shadow epoch therefore adds an eye delta per view, as planned.

The cost it measured is the baseline S5 will be judged against: 2.3 ms of render-thread CPU per frame in
the exterior (2.2 of it in cascade 1's 2,569 draws), 0.5 ms for two point lights at night.

### The two-phase frame

Shadow views are drawn *inside* `Main_RenderShadowMaps`, and the main camera's passes are registered by
jobs that run concurrently with them - they are complete only when that call returns. So an object record
a shadow epoch can read has to exist before it, and the accumulator's half of the record cannot.
`SceneStore::BuildFrame` now takes a phase:

-   **`Phase::Scene`**, from `DrawcallLimitFix::BeforeShadowMaps`: the tracked walk, eligibility, the
    geometry slots and their buffer resolve, transforms, bounds, bone palettes, and one object record per
    eligible object - with no pipeline, no material, `kObjectNoBindings` and `kObjectNativeVisible` clear.
    Object indices are fixed for the frame from here.
-   **`Phase::Accumulate`**, at `EarlyPrepass` as before: the capture drain, the pipeline and material
    slots, the per-frame lighting template, shading, light lists, decal order and `kObjectNativeVisible`,
    patched into those records by object index.

Three things this split taught:

-   **The scene phase must not decide anything that needs the pass.** The decal rule is the one such
    rule - a decal's group comes from its accumulated pass's accumulation hint - and rejecting on it
    before the shadow maps left 452 accumulated objects without records and dropped claims from 1712 to
    1531. Decal verdicts are now deferred: the record is built, and the accumulate phase takes the verdict
    again with the pass in hand (`DeferredToAccumulate`).
-   **The per-frame classification still belongs to every frame.** The scene phase's verdict is cached
    for up to 64 frames, which is right for "is this object drawable at all" and wrong for hidden, actor
    and fading. The accumulate phase re-runs `ClassifyFrame` even when the derived cache hits, or an
    object that has just been hidden keeps its bindings.
-   **The second phase should iterate the passes, not the tracked set.** Walking all 10,053 tracked
    objects again to look up 1,700 accumulated ones cost 0.4 ms of pure loop; iterating
    `accumulatedPasses` and finding the tracked entry costs nothing measurable.

Measured against the A1/A2 build at the same place, every main-pass gate holds: claims 1719 (against
1712) with 0 claimed-but-not-drawn, capture parity clean in steady state, draw parity 0 of 13,843,
derived cache 1719 served / 0 differ, 0 slot violations.

| | A1/A2 (one phase) | two phases |
|---|---|---|
| tables CPU per frame | 2.12 ms | 2.77 ms (scene 1.60, accumulate 1.17) |
| object records | 4,937 | 5,358 |
| geometry slots | 645 | 953 |

The extra 0.65 ms is the scene phase giving records, geometry slots and transforms to every eligible
object rather than bounds alone to the ~3,200 the main camera's culling had rejected. That is the work
the shadow epochs exist to consume, and it is paid once for all views.

## Shadow views, step two: the views drawn, and the counters that say so

Step S2 of the shadow plan gives DCLF the sun's cascades - every view the engine draws with render mode
0xD-0xF that is not a focus shadow - behind `CS_DCLF_SHADOWS=1`, with static ownership of their casters
behind `CS_DCLF_SHADOW_OWNERSHIP=static`. Shadow correctness is judged by counters and readbacks alone
for now: Community Shaders' shadows are broken in general at the time of writing, so a screenshot says
nothing about DCLF's part in them.

### What was built

-   **`ShadowViews`** (`DrawcallLimitFix/ShadowViews.{h,cpp}`): the frame's views from the shadow scene
    node's caster array, in the engine's order, keyed by accumulator (what the 0x2A hook is called on)
    and by batch renderer (what the engine registers the view's Utility passes with). Each view also
    carries the accumulator's render mode from its last draw, which is what attributes a registration to
    a mode's claim set. The batch renderers of the non-focus views are published to `PassCapture` per
    frame, as the main camera's are.
-   **The shadow classification** in the scene phase: `ShadowCasterReject` (the engine's rule, 0 false
    rejections over ~1M registrations in step one) sets `kObjectNoShadow`; `ShadowUtilityTechnique`
    gives every object its Utility technique without the mode bits; two-sided and the alpha-test
    threshold are read from the property in the scene phase, because the shadow epoch needs them before
    the accumulate phase runs.
-   **Utility programs and the shadow pipeline set**: `ShaderPrograms::FindShadow` compiles
    `Utility.hlsl` with `DCLF_BINDLESS` per (technique | mode bits); the VS takes `World` from the
    object record plus a per-view eye delta (`DCLFEyeDelta`, b0 c2), bones from the bone rows with the
    same delta, `TreeParams` from the record, and `AlphaTestRefRS` from the record's threshold bits.
    `DrawPipelines::FindShadow` keeps a second pipeline set and indirect signature: depth only, LESS with
    writes, the engine's rasterizer bias for the key's bias mode, culling from the key, the shadow map's
    DSV format (`D16_UNORM`).
-   **Capture, then one epoch.** The 0x2A hook (`BSShaderAccumulator::FinishAccumulatingPreResolveDepth`)
    only *captures* a view after its native draws: target and slice from the renderer state, the
    viewport, `posAdjust`, the PerTechnique block (parabola parameters from the two globals, the eye
    delta) and a copy of `VS_PerFrame` as the engine wrote it for the view. `ExecuteShadowFrame`, at
    `AfterShadowMaps`, then runs *one* graph epoch for every captured view: the object records, bone rows,
    geometry table and binding records once; the draw inputs once per render mode present (a caster with a
    ready pipeline under that mode); per view a slot holding its two blocks at the head of the constant
    arena, its own copy of the binding records naming them, its sequence and count buffers, one
    `BuildDrawsCS` dispatch (frustum only, single phase, no engine-visibility gate) and one
    `ExecuteIndirect` into its slice through a per-slice DSV at the view's viewport origin
    (`rhi::PassBeginInfo::x,y`, added to BasicRHI for this).
-   **The volumetric copy.** The engine draws each cascade twice: into `kSHADOWMAPS_ESRAM` (4096², two
    slices) and again, through the same batch renderer, into `kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM`
    (512², two slices) for the volumetric lighting. A caster withheld from the renderer is missing from
    both draws, so under ownership DCLF must draw both: target 3 is the third imported shadow depth
    target. Before it was, exactly two views a frame reported "not ready (depth)".
-   **Static shadow ownership.** After a successful epoch the inputs of each mode become that mode's
    claim set (`PassCapture::PublishShadowClaims`); the registration hook withholds a Utility pass whose
    batch renderer belongs to a view of that mode and whose geometry is claimed. The exterior withholds
    ~2,150 of the cascades' registrations a frame with 5,176 claimed; the engine still draws the rest
    natively (actors, grass, LOD, terrain - nothing DCLF has a record for).
-   **Live toggles.** Every non-default feature - hybrid, ownership, the culling mode and input, the
    object classes, the shadow views and their ownership, and the diagnostic modes - is a `Toggles`
    entry seeded from its switch and edited in the DCLF menu. The render thread applies the requested
    set once per frame at `BeforeShadowMaps`; a change to a toggle that enters the classification drops
    every cached verdict and derivation (`SceneStore::InvalidateVerdicts`). The shadow system has its own
    switch there so it can be excluded while everything else is tested.

### The counters

Riverwood exterior, the standard switch set plus shadows and shadow ownership, 120 s:

| counter | value |
|---|---|
| views offered per frame | 5 (two cascades into target 2, the same two into target 3, one focus view) |
| views drawn / not ready / focus left native | 4 / 0 / 1 |
| inputs per mode (0xE) | 5,176 with 0 lacking a pipeline, 0 lacking a texture; 163 binding records |
| GPU culling, cascade 0 (sampled) | 5,176 tested, 111 drawn, 5,065 outside the frustum |
| GPU culling, cascade 1 (sampled) | 5,176 tested, 2,308 drawn, 2,868 outside |
| shadow ownership | 2,149 clamped passes withheld per frame, 5,176 claimed, 0 views not ready |
| main-pass gates | claims 1,712, 0 claimed-but-not-drawn in steady state (one transient of 2 during churn) |
| CPU per frame | 1.65 ms: capture 0.01, prepare 0.38, inputs 0.34, blocks 0.05, graph 0.87, claims 0.14 |

Two lessons the counters taught:

-   **The graph's execution is a per-epoch floor.** One epoch per view cost 4.4-6.0 ms a frame, of
    which 3.5-5.1 ms was the graph's own compile/prepare/record - the main epochs show the same ~0.9 ms
    "graph execute" part. One epoch for all views brought the shadow path to 1.65 ms with the same four
    views drawn. Anything that runs per view has to be a dispatch or a pass inside one epoch, never an
    epoch.
-   **A "not ready" count needs a reason.** The two unexplained not-ready views a frame were the
    volumetric copies; the per-reason counters (`setup`, `pipelines`, `tables`, `depth`, `epoch`,
    `capacity`) said `depth` and a once-per-target log line said target 3.

The engine's own shadow-map CPU with the probe on was 2.7 ms a frame before ownership; what the native
loop saves under ownership is not measured yet (step S5).

### Open, carried to S3-S5

Spot lights (mode 0xD, the clamped permutation) are built but untested for lack of a spot-lit cell in the
runs so far; parabolic lights (S3) and the focus views (S4) stay native; `HighDetailRange` is zero, the
depth bias uses mode 0 only, the samplers are the default wrap/anisotropic; the object records are rebuilt
per frame for the shadow epoch (0.38 ms) although the scene phase has them; the inputs cost two hash
lookups per caster per mode (0.34 ms); the claim set is rebuilt per frame (0.14 ms); the hole detector
counts not-ready views rather than withheld passes per view. The shadow image parity gate from the plan is
deferred until Community Shaders' shadows are themselves correct.

## Payloads built off the render thread

The GPU idle trace (`CS_GPU_IDLE_TRACE`) showed the GPU waiting 2-5 ms a ~24 ms frame for the render
thread, most of it DCLF's own CPU work at points where the GPU had already drained everything submitted:
0.8-1.5 ms before the main-opaque epoch, 0.8-1.4 ms before the Z-prepass, 0.2-0.5 ms before the shadow
epoch and 0.3-0.6 ms for the scene tables at the start of the frame. Reflex keeps the queue deliberately
shallow, so queue depth cannot hide those gaps. The work has to leave the render thread's path between
dependent GPU work. The work is split into phases, each gated on its own. All five have landed.

### The contract: prepare, build, commit

-   **Prepare** (render thread, at the kick): everything the build reads that is not fixed until the join
    is copied into an inputs struct: frame, eye pair, render flags, frame-slot masks, the resources'
    addresses, and the tables' and lookups' generations.
-   **Build** (`BuildMainPayload`, `BuildShadowPayload`): a pure function of the inputs, the tables and
    the lookups. It makes no engine, service or D3D11 call, and it writes only its payload. `probe` runs
    it twice for exactly this reason.
-   **Commit** (render thread, inside the epoch's `beforePrepare`): checks that the job's inputs equal the
    epoch's (`SameInputs`; a mismatch is *stale* and the epoch builds inline), then copies the frame
    blocks into their slots, patches the frame textures, uploads, and hands the frame to the graph.
    `BUFFER_UPLOAD`, the descriptor service and `ConstantEvaluator` stay on this thread.

**Lookups** (`DrawcallLimitFix/Lookups.h`, owned by `SceneStore`) replace what the build used to ask
services for. The render thread fills them where the service is legal: pipelines (set index, constant
tables, register usage per variant) at `EarlyPrepass`, material textures, samplers and projected
textures at the epoch's commit, and shadow pipelines and textures at the shadow commit. Frame constant
blocks moved into `frameConstants`, a buffer of fixed 64 KB slots (`FrameSlotOffset(stage, register)`),
so a record can name a block's address before the block exists.

**The worker** (`DrawcallLimitFix/AsyncWorker.{h,cpp}`) is one dedicated thread, `CS DCLF worker`, with
a small FIFO and a bounded join. A job that is late (`CS_DCLF_ASYNC_WAIT_MS`), fails or is stale falls
back to the inline build, and the worker's payload is dropped. `EndFrame` counts any job still
outstanding at the end of the frame as `leaked`. `SetActive(false)` drains the worker. ORG's task
service and the shader compilation pool were ruled out: the first is what `ExecuteFrame` fans onto while
the render thread waits on it, and the second is saturated exactly when a new cell loads.

### Phase 1 gate: the split, still inline

Every parity line matched the pre-split build: record dedup, bindless record, `BuildDraws` and decal
parity OK; capture parity at its standing material residue (the same windows as before, ±3%);
`0 claimed but not drawn`; shadow `0 views not ready`; `derived cache 0 differ, 0 slot violations`;
live toggle 900:960 clean. The idle trace did not change beyond noise. The shadow epoch's lookup
refresh first cost 0.8-1.0 ms because it walked the shadow textures per view. Collecting the alpha-tested
casters' diffuse textures once in the scene phase (`Tables::shadowTextureSet`) removed that cost.

### Phase 2: the colour job

The colour epoch replays the Z-prepass's eye and constants (the hybrid path), so its inputs are final at
`DrawcallLimitFix::Prepass`, right after `RefreshFrameConstants`, the frame's last writer of the tables.
The job is kicked there (`IndirectDraws::KickColourBuild`) and joined in `RunEpoch(MainOpaque)`. That
leaves 1.5-2 ms of native rendering between the kick and the join. The frame-slot masks the epoch will
supply are predicted from the previous colour epoch and checked like every other input. They change only
when the set of bound frame blocks changes, which happens on the first frame of a cell and on a load.

The join comes before the epoch refreshes the material lookups, and the order matters. The worker reads
the lookups until it finishes, so a refresh that grows them underneath the job is a data race. A refresh
that adds or changes an entry bumps the lookups' generation, which marks the job stale.

Gate (exterior → Dragonsreach → Riverwood, live toggle at 900:960, save and load, ~2,700 frames):

| check | `probe` | `on` |
|---|---|---|
| epochs using the worker's build | 295-300 of 300 per window | 295-300 of 300 per window |
| stale | 1-2 at `coc`/`load` (predicted masks), otherwise 0 | the same |
| late / failed / cancelled / leaked | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| payload comparison | 295-300 compared, **0 differ** | - |
| other parity lines and ownership | unchanged from Phase 1 | `0 claimed but not drawn` in every window |

GPU idle attributed to the main-opaque epoch (`main opaque inputs` + `main opaque` in the
`[GpuIdle] summary`, ms per frame, 300-frame windows): **0.42-1.55 before, 0.04-0.55 after**. The rest
is ORG's prepare/record/submit floor and the join's wait. The Z-prepass epoch is still built inline, and
its share swings by ±0.5 ms from run to run.

### Phase 3: the Z-prepass job, on a predicted eye

Between `EarlyPrepass` and the end of `Main_RenderDepth` nothing writes the tables: the accumulate phase
is their last writer, and `RefreshFrameConstants` runs later, at `Prepass`. The Z-prepass job is kicked
at the end of `EarlyPrepass` (`IndirectDraws::KickZPrepassBuild`) and joined by `RunEpoch(ZPrepass)`.
The only input not yet known at the kick is the eye, which the epoch captures from `posAdjust`. At
`EarlyPrepass`, `posAdjust` still holds the shadow cameras' eye, so the job uses a prediction:
`RE::Main::WorldRootCamera()->world.translate` for the eye, and last frame's captured eye for the
previous eye. The epoch compares both exactly with its capture, and a miss makes the job stale.
The miss count is reported on its own line (`[DCLF] async zprepass eye`). `drewLastFrame`, the gate the
Z-prepass uses without withholding, is snapshotted at the kick. The render thread's colour epoch is the
only writer of `drawnFrame`, and it runs after the join.

Two defects surfaced on the way, and both are fixed:

-   **One job could cancel another.** Dropping a job used `CancelPending()`, which empties the whole
    queue. With two jobs a frame, dropping the colour job could cancel a queued Z-prepass job.
    `AsyncWorker::Cancel(handle)` ends one job: it is dequeued, or waited for if it is already running.
-   **A newly resolved lookup did not make a job stale.** The lookups' generation was bumped only when an
    already-resolved entry changed. A material resolved for the first time in the epoch's refresh left
    the job looking current, yet the job had deferred those draws. The probe caught it right after a
    `coc` (`constants: 4912 vs 82720 bytes`). Under ownership that would be one frame of missing depth
    for the claimed objects. Any change a build can observe now bumps the generation, including an
    entry resolved for the first time. As a result, the Z job goes stale for 2-3 frames after a cell
    change and while textures stream in at startup.

`CS_DCLF_ASYNC_EYE` was dropped: the prediction has not missed once, and `CS_DCLF_ASYNC_JOBS` without
`zprepass` already keeps the Z-prepass inline.

Gate (the same route and switches as Phase 2):

| check | `probe` | `on` |
|---|---|---|
| Z-prepass epochs using the worker's build | 296-300 of 300 per window | 285-300 of 300 per window |
| stale | 3 at each `coc` | 2-11 (startup streaming, `coc`, `load`), 0 in steady state |
| predicted eye missed | 0 | 0 |
| late / failed / cancelled / leaked | 0 | 0 |
| payload comparison | 296-300 compared, **0 differ** (colour and Z-prepass) | - |
| other parity lines | record dedup, bindless record, `BuildDraws` OK; derived cache 0 differ, 0 slot violations | - |
| ownership | 0 claimed but not drawn | 3 in the startup window only (the standing transient: every run since the Phase 0 baseline has 1-3) |

GPU idle attributed to the Z-prepass epoch (`Z-prepass inputs` + `Z-prepass`, ms per frame, 300-frame
windows): **0.61-1.40 before, 0.06-0.40 after**. The main-opaque share stays at 0.01-0.43.

### Phase 4: the shadow inputs job

The shadow epoch's inputs are final at `BeforeShadowMaps`, right after the scene phase and
`BeginShadowFrame` (the reference eye). Between there and `AfterShadowMaps` nothing writes the tables:
every writer is in the scene phase (before) or the accumulate phase (after), and `ExecuteShadowView`
and the shadow probe only read. The job is kicked there (`IndirectDraws::KickShadowBuild`) and joined by
`ExecuteShadowFrame`. Its window is the engine's entire shadow-map pass. Two inputs are unknown until
the views are captured: the render modes, and the DSV format the shadow pipelines are looked up for.
The job builds for last frame's modes. The epoch compares the modes with its own set, and a DSV format
change shows up as a changed shadow pipeline lookup, which bumps the generation. Either makes the job
stale. `SharedData` and `FeatureData` are copied at the kick and compared as well.

Gate (same route and switches):

| check | `probe` | `on` |
|---|---|---|
| shadow epochs using the worker's build | 290-300 of 300 per window | 290-300 of 300 per window |
| stale | 6 at startup (lookups resolving), 1-3 at a mode change (`coc` into Dragonsreach's paraboloid views and back) | the same |
| late / failed / cancelled / leaked | 0 | 0 |
| payload comparison | 290-300 compared, **0 differ**, in both the cascades (0xE) and the paraboloid views (0xF) | - |
| shadow ownership | 0 views not ready outside the startup window (the standing 12) | the same |
| join wait | ≤0.001 ms average | ≤0.001 ms average |

Shadow epoch CPU per frame on the render thread: **1.93 ms before, 1.14 ms after** (preparing 0.79 →
0.04 ms). What is left is the graph's own compile/prepare/record (0.95 ms) and publishing the claims
(0.15 ms). GPU idle attributed to the shadow epoch is 0.22-0.34 ms/frame, against 0.16-0.54 before:
the ORG floor.

### Phase 5: the scene walk

The scene phase cost the render thread 1.8-2.3 ms a frame in the exterior (26 ms on the frame after a
load), more than any epoch. `BuildScenePhase` is now split into three parts:

-   **Prologue** (render thread, `BeforeShadowMaps`): the load-screen branch, the frame globals,
    `SweepSlots`, the lighting shader, and the iteration order over `tracked`. When the walk is going to
    the worker, `PrepareSceneJob` also makes the two calls the walk would otherwise make that only the
    render thread may: `GpuResources::Touch` for every geometry slot used last frame, and the engine's
    palette update (`UpdateSkin`, AE `0xe4ff90`) for last frame's skinned objects.
-   **Walk** (`SceneWalk`, on the worker or inline): today's loop, unchanged except for what it may not do
    off the render thread. A geometry slot that is new, changed, or not touched ahead, and a skin the
    prologue did not update, count as *misses* instead. That happens on the frame after a cell change
    (thousands of misses) and when a new skinned object becomes eligible.
-   **Join** (`JoinScenePhase`, render thread): at `AfterShadowMaps`, so the window is the engine's whole
    shadow-map pass. `EarlyPrepass` and Present also call it, in case the frame drew no shadow maps. A
    walk with misses is replaced by an inline walk, which resolves them. The shadow build queued behind the
    walk is first allowed to finish (`AsyncWorker::WaitIdle`), then made stale by `GetSceneRebuilds`.

The window was audited rather than assumed:

-   `tracked` changes only in `ProcessEvents`, at Present.
-   The skip hook returns before `FindObject` for every pass outside the depth and deferred passes, so
    the native shadow draws never read `objectIndex`.
-   `ExecuteShadowView` no longer reads the tables. It only checks whether they are ready, and
    `ExecuteShadowFrame` makes the same check after the join.
-   The shadow probe reads the tables in the window, so the walk stays inline while
    `CS_DCLF_SHADOW_PROBE=1`.
-   The engine data the walk reads (world and previous-world transforms, bounds, runtime data, properties,
    materials, fade nodes, the parent chain, palettes) is the data the render thread read at this same
    point before. Its only calls are side-effect-free virtuals (`GetRTTI`, `GetType`, `GetUserData`,
    `GetFeature`).

`probe` re-walks on the render thread at the join and compares the per-object arrays byte for byte,
including the slots-used vector. A difference would mean either a walk that is not a function of its
inputs, or engine data that changed during the shadow maps.

Gate (same route and switches):

| check | `probe` | `on` |
|---|---|---|
| walks used | 299-300 of 300 per window | 299-300 of 300 per window |
| rebuilt inline | 1 per cell change (2,266-5,349 geometry misses, 0-12 skin misses) | the same |
| slot touches failed ahead | 0 | 0 |
| comparison | 299-300 compared, **0 differ** | - |
| every other job | 0 differ, 0 late, 0 leaked | 0 late, 0 leaked |
| parity and ownership | slot probe 0 differ; bone palettes 0 differ; derived cache 0 differ, 0 slot violations; capture parity at its standing residue; 0 claimed but not drawn | 0 claimed but not drawn except 4 in the startup window (the standing transient) |

Render-thread CPU of the scene phase: **1.8-2.3 ms before, 0.14-0.18 ms after** (the prologue).
Scene-phase GPU idle: 0.06-0.64 ms/frame before, ≤0.04 after.

What moved rather than disappeared: the walk (1.1-1.5 ms on the worker) and the shadow build chained
behind it (0.5-0.7 ms) take longer than the engine's shadow-map pass. The render thread therefore waits
0.6-0.9 ms on average at `AfterShadowMaps` (0.3-0.6 of it for the shadow build). The GPU has the shadow
maps to draw during that wait, so little of it shows as idle.

### All five jobs, `on`

GPU idle per frame, 300-frame windows, the same route (exterior → Dragonsreach → Riverwood, save, load):

| window | idle before (Phase 0) | idle after | `CS DCLF` share before | after |
|---|---|---|---|---|
| exterior, startup | 0.81 | 0.37 | 0.71 | 0.32 |
| Dragonsreach | 2.34-2.98 | 1.48-3.77 | 1.32-2.47 | 0.47-0.83 |
| Riverwood and after save/load | 2.02-4.96 | 0.27-2.04 | 1.66-4.03 | 0.22-1.33 |

The `CS DCLF` share that remains is ORG's own prepare/record/submit per epoch, plus the joins' waits.
In Dragonsreach the total idle did not fall with the DCLF share: what remains there is not DCLF's.
Frame rate is not a usable signal on this route: medians ranged from 36.5 to 46.1 fps across runs with
no trend by phase.

Open:

-   The worker's builds take 0.8-2.0 ms, against ~1.0 ms for the same build inline. The 9800X3D has no
    hybrid cores, so the likely cause is contention with the render thread's native rendering. The colour
    join therefore still waits 0.1-0.5 ms on average. The Z-prepass join barely waits (≤0.02 ms average)
    because its window is longer. Next is a cheaper build: the two main epochs still build their object
    records, bone rows and geometry table separately.
-   The join at `AfterShadowMaps` waits 0.6-0.9 ms. The scene walk and the shadow build need a longer
    window than the shadow-map pass, or a cheaper walk.
-   The standing startup transient of 1-4 claimed-but-not-drawn objects in the first 300 frames: present
    since the Phase 0 baseline, and not yet traced.

## Epoch costs: measured, and the first two cuts

`CS_ORG_EPOCH_STATS=1` now splits each epoch's render-thread time by segment and by phase of ORG's host frame
(`PersistentGraphHost::LastFrameTimings`, `RenderGraph::LastPersistentExecuteTimings`), and counts the queue
submissions the graph hands to DXVK. Baseline after the async work (µs per epoch, exterior):

| epoch | total | inputs (commit + join) | prepare | admission | record | other ORG | submissions |
|---|---|---|---|---|---|---|---|
| LLF light culling | 579 | 6 | 245 | 84 | 110 | 134 | 5 |
| Z-prepass | 822 | 159 | 216 | 86 | 221 | 140 | 5 |
| main opaque | 1,296 | 634 | 175 | 89 | 237 | 161 | 5 |
| shadow views | 1,354 | 677 | 219 | 95 | 213 | 150 | 5 |
| debug view | 300 | 0 | 59 | 63 | 64 | 114 | 3 |

What that showed:

-   **The inputs phase is mostly the join wait.** The colour join waits 0.52 ms and the shadow join 0.57 ms,
    because the worker's builds are slower than their windows. The table work (the next steps) removes this.
-   **ORG costs 0.55-0.65 ms per epoch** whatever the epoch does: prepare, admission and record dominate,
    because every epoch prepares and records every pass of every feature.
-   **`HostWait` is cheap** (25-50 µs). The GPU is not what holds the render thread at an epoch's start.
-   **The debug-view epoch ran every frame** on the hybrid path: its targets are imported there anyway, and their
    presence was the only gate. It is now gated on the debug-view toggle, which removes 0.3 ms of render thread and
    three submissions a frame.
-   **DXVK held the pre-epoch D3D11 work back.** It closes its command list only when the epoch's first
    submission is enqueued, after ORG has prepared and recorded. So the D3D11 commands issued just before an epoch
    sat on the CPU for 0.5-1 ms while the GPU ran dry. `ExecuteEpoch` now flushes the immediate context first
    (`CS_ORG_EARLY_FLUSH=0` restores the old behaviour); the enqueue then finds nothing pending, so the
    submission count is unchanged.

A/B with the same build (Riverwood windows, GPU idle attributed to `CS DCLF`, ms per frame): early flush off
1.23-2.17, on **0.56-1.08**; total idle 2.28-4.70 against 0.97-2.24. What remains is the main-opaque epoch
(ORG 0.26-0.35, the colour join 0.19-0.32); shadow and Z-prepass are down to 0.05-0.23.

## Less work per build: the eye on the GPU, a build cache, no per-frame object map

### The eye moved to the GPU

The object records and bone rows were made eye-relative on the CPU (`StoreRelative`, `PackBoneRows`). That is
why the shadow, Z-prepass and colour builds could not share them, and why the Z-prepass job needed a predicted
eye. Every DCLF draw already receives its epoch's `VS_PerFrame` (b12). A check on every epoch and every shadow view
confirmed, bit for bit, that its `CameraPosAdjust` (c40) is the eye the records were made relative to, and its
`CameraPreviousPosAdjust` (c41) the previous eye: 6,000 main epochs and 13,200 shadow views, 0 different.

-   Records and bone rows are absolute now. `Lighting.hlsl` subtracts `BonesPivot`/`PreviousBonesPivot`
    (c40/c41 of its own `VS_PerFrame`, now declared under `DCLF_BINDLESS` as well as `SKINNED`) with `precise`.
    That is the same single float subtraction the CPU did, so the depth and colour epochs agree to the bit.
-   `Skinned::GetBoneTransformMatrixBindless` takes the pivot and subtracts it per bone before the blend, as the
    engine's own `GetBoneTransformMatrix` does.
-   `Utility.hlsl` subtracts the drawing view's own `CameraPosAdjust`, so `DCLFEyeDelta` is no longer read (left
    zero). The shadow result is now closer to the engine's own, which subtracts the view's eye from the absolute
    matrix, than the old two-step delta was.
-   The bindless parity check makes the absolute record relative with the same subtraction before comparing:
    `bindless record parity OK` over 245M components.
-   A bindless build no longer reads the eye unless the parity checks are on (`BuildReadsEye`), so the eye is
    not an input of the Z-prepass job and cannot make it stale.

Gate: capture parity at its standing residue, `BuildDraws parity OK`, `0 claimed but not drawn` (the Z/colour
EQUAL test holds), every job `probe 0 differ`.

### A build cache for the (material, pipeline) pairs

`BuildCache` (`CS_DCLF_BUILD_CACHE`, default on, `=0` off), one per main epoch kind, keeps each pair's resolved
texture and sampler indices and packed PerMaterial groups across frames, and each pipeline's packed PerTechnique
groups and PerGeometry template. An entry keeps a copy of every input it was derived from and is reused only when
this build's inputs are byte-identical, so it cannot serve a stale value. `CS_DCLF_ASYNC=probe` checks that every
epoch: the worker's build uses the cache and the inline build does not, and their bytes are compared.

Three things the numbers corrected:

-   **The drifting IBL floats.** `RefreshMaterialPatch` writes them into every material each frame, which
    rebuilt every pair every frame (hit rate ~0%). The signature now leaves out the patched positions
    (`MainInputs::materialPatchedFloats`), and a reused PS group gets this frame's values written at their packed
    offsets (`PackedPositionOf`), exactly as `PackConstantGroup` would.
-   **The signature must be cheaper than what it saves.** Copying the 2.3 KB material record into each pair's
    signature cost more than the texture loops it replaced. `Tables::materialVersion`, a session-unique number
    set whenever a slot's record is written, apart from the patch, now stands in for it.
-   **Pipelines still rebuild in the exterior.** Their evaluated PerTechnique and PerGeometry constants change
    every frame (EyePosition, fog). There are only 48 of them, so they are left alone.

Result: 99.5-100% of pairs reused. The colour build takes 1.22 ms instead of 1.50, its join waits 0.21 ms instead
of 0.53, and the GPU idle during `main opaque inputs` fell from 0.19-0.32 to 0.04-0.09 ms per frame.

### No per-frame object map

The scene walk rebuilt a geometry-to-index hash map every frame for about 5,300 objects. The index now lives in
the object's `Tracked` entry (`objectStamp`, `objectId`), valid while it matches the walk's stamp. The accumulate
phase already holds the entry, and `FindObject` looks it up in the persistent tracked map. The accumulate phase got
0.1 ms faster; the walk did not. Its cost is reading scattered engine memory for every object (world and previous
world, bounds, properties, the parent chain), not the tables it writes: the same conclusion as "the transform
witness cannot pay for itself" above. Skipping unchanged objects would still have to read them to know.

### Where the frame's DCLF bubbles stand

After early flush, the debug-view fix, the GPU eye and the build cache, DCLF-attributed GPU idle is about 1 ms per
frame in the exterior, split roughly as follows:

-   the shadow epoch, 0.5-0.7 ms: ORG, plus the wait at `AfterShadowMaps` for the scene walk (1.1-1.6 ms on the
    worker) and the shadow build chained behind it;
-   ORG in the main-opaque epoch, 0.3-0.45 ms;
-   the Z-prepass, 0.1-0.2 ms.

ORG costs 0.55-0.65 ms of render thread per epoch whatever the epoch does, because every epoch prepares, admits
and records every pass of every feature. That is the next lever: epoch variants (plan step O2), and after them
worker-side recording.

## Epochs: each submission point runs only its own passes

Every ORG epoch used to prepare, admit, record and submit every pass of every feature. The shadow epoch prepared
LLF's culling passes and DCLF's colour passes, which then returned empty work. ORG now has host epochs
(`extern/OpenRenderGraph/docs/persistent-epochs.md`).

-   **One epoch-aware compile.** The persistent program compiles once over the whole frame, epochs in frame
    order, so scheduling and transient aliasing see every segment's lifetimes. Transients in different epochs
    can share memory.
-   **Per-epoch executables.** Each epoch also gets an executable compiled from its own passes over the same
    bindings and placements. It is what runs at that epoch, and its entry states come from the admission
    ledger, so an epoch that did not run is harmless.

This was chosen over one compile per epoch because that would have given each epoch its own alias plan (no
memory shared across epochs) and turned every resource crossing epochs into an external one.

CS's side (`CS_ORG_EPOCHS`, default on, `=0` for the old behaviour):

-   Each segment is an epoch, in frame order: shadow views, Z-prepass, light culling, main opaque, debug view.
-   Every pass declares its segment.
-   The Z-prepass has its own `cs.dclf.z.build-draws` and `cs.dclf.z.depth` instances, where before one
    `build-draws`/`main-opaque` pair served both the depth and colour segments.

One thing the first run found: ORG derives hazards in authored (registration) order. Across epochs that order
meant nothing, and the epoch edges then closed a cycle ("Captured dependency graph contains a cycle"). The
program now reassigns authored order by epoch rank, then registration order.

Measured, same build, `CS_ORG_EPOCHS=0` against on. ORG's render-thread µs per epoch, everything but the feature
inputs:

| epoch | before | after |
|---|---|---|
| LLF light culling | 638 | 491 |
| Z-prepass | 712 | 598 |
| main opaque | 675 | 564 |
| shadow views | 689 | 591 |

That is about 0.5 ms of render thread per frame. Admission, recording and commit fell with the pass count. The
rest of each epoch is fixed cost, which the split does not touch:

-   ORG's per-frame polls and statistics setup, about 100 µs;
-   the host upload pass, 44 µs every epoch;
-   retirement and command-list acquisition;
-   five queue submissions per epoch.

ORG's per-pass preparation is only 3-19 µs a pass. The idle trace does not separate the two runs beyond noise.

Gate: probe 0 differ for every job, `BuildDraws parity OK`, capture parity at its residue, `0 claimed but not
drawn`, and save/load and the live toggle clean. ORG's test suite passes in the CS embedding configuration,
including a new epoch test in `PersistentGraphTests`.

What remains per epoch is fixed ORG cost, which the next steps in the plan address:

-   one submission per epoch instead of five (O1c);
-   invocations that are reused rather than re-prepared, with no per-epoch serial in their revisions (O3);
-   retained command buffers (O4);
-   worker-side recording (O5).
