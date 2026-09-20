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
| `CS_DCLF_TEST_COMMANDS=<frame>:<command>;…` | Test runs: run each console command on the main thread once the main pass has run that many frames (loading screens do not count), for coverage runs from the auto-loaded save. |

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
