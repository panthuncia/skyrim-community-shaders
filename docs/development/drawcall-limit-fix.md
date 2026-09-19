# Drawcall Limit Fix

Drawcall Limit Fix (DCLF) replaces the game's per-object opaque draw loop with GPU-driven indirect draws
that the render graph executes on DXVK's Vulkan device (see [OpenRenderGraph on DXVK](./render-graph.md)).
It is built in phases:

1.  **Scene capture** (current): CPU tables describing every object DCLF will draw.
2.  **Indirect pipeline**: Vulkan device-generated commands, shaders recompiled for the render graph,
    tables uploaded to the GPU.
3.  **Integration and parity**: the native loop skips what DCLF draws; the images must match.
4.  **GPU culling**: two-pass object occlusion culling at the Z-prepass.
5.  **Cutting back the native loop** and its CPU occlusion culling.

What the game binary does, as far as DCLF depends on it, is recorded in
[Skyrim engine notes](./skyrim-engine-notes.md).

## Phase 1: scene capture

Source: `src/Features/DrawcallLimitFix.{h,cpp}` and `src/Features/DrawcallLimitFix/`.

### Which objects

Static rigid geometry: a `BSTriShape` (not dynamic, multi-index or LOD) with no skin, a
`BSLightingShaderProperty`, a technique DCLF supports (None, Envmap, Glowmap, Parallax, including TruePBR
materials), no alpha blending, no decal flags, no projected UV or refraction, and not under a
`NiSwitchNode` or `BSOrderedNode`. Per frame it must also be visible (not app-culled or hidden up to its
cell node) and fully faded in. Objects whose LOD fade inputs are undefined in the engine (see
`LightingDescriptors.cpp`) stay native. Anything else stays on the native loop, which draws it as before.

The reasons are counted per frame (`CS_DCLF_STATS=1`, or the feature's settings page).

### Tracking

`SceneTracker` detours the `NiNode` child functions (`AttachChild`, `DetachChild*`, `SetAt*`) and pushes
events onto a lock-free stack from whatever thread attaches or detaches. Detach events carry the
geometry leaves collected on that thread while the subtree is intact.

`SceneStore` belongs to the render thread. Every Present (`Feature::Reset`, so also in menus) it:

1.  refreshes the set of tracked cell category nodes: Static, Dynamic and MultiBound of each attached cell
    (interior cell or loaded grid). New cells are scanned and removed cells dropped;
2.  applies the queued events in order;
3.  re-checks a slice of tracked geometry against its category node, as a safety net for missed detaches.

### Tables

At the start of the main pass (`Feature::Prepass`, when `Deferred::StartDeferred` runs) `BuildFrame`
rebuilds, for the eligible set:

| Table | Content |
| --- | --- |
| `objects` (`ObjectRecord`, 128 bytes, GPU layout) | world and previous world 3×4, world bounding sphere, geometry / material / pipeline indices, flags (alpha test, two-sided, alpha threshold) |
| `geometries` (`GeometryRecord`) | the game's vertex and index buffer, vertex description and stride, index count |
| `pipelines` (`PipelineKey`) | final vertex and pixel shader descriptors (as `State::ModifyShaderLookup` produces them for the deferred pass), two-sided, and the raw pass descriptor |
| `geometryConstants` (per pipeline) | the per-frame `PerGeometry` constants for that pass descriptor |
| `materials` (`MaterialRecord`) | the `PerMaterial` constants, textures and address modes for each (material, pass descriptor) |
| `shading` (`ObjectShading`, 32 bytes) | the per-object `PerGeometry` pixel constants (LOD fades, alpha, emissive colour, SSR specular) |
| `draws` (`DrawSequence`, 48 bytes) | one indirect draw per object in the device-generated-commands token order, addresses filled in Phase 2 |

The shader descriptors are derived from the property flags (`LightingDescriptors.cpp` follows
`GetRenderPasses` and `SetupTechnique`, including the specular and envmap LOD fades and TruePBR's
changes).

Constants are not ported: `ConstantEvaluator` runs the engine's own `BSLightingShader::SetupMaterial` and
`SetupGeometry` outside the render loop against stand-in shader objects (engine notes: "Evaluating the
Setup functions outside the render loop"). The results therefore include every Community Shaders hook on
those functions. Materials are evaluated once per (material, pass descriptor) per frame, and geometry
constants once per pipeline per frame. Only World, PreviousWorld, the LOD fades, alpha, emissive colour
and SSR specular are per object; they are stored in `objects` and `shading`.

### Checking it: capture parity

`CS_DCLF_CAPTURE_PARITY=1` compares every native main-pass lighting draw of an object in the tables with
the tables, and logs every 300 frames:

-   `capture parity OK|MISMATCH`: shader descriptors, world transform, `PerMaterial` constants, textures and
    address modes, and `PerGeometry` constants (with the per-object values applied). Native constants are
    captured from the Map/Unmap detours; only components the engine writes are compared. Also counted:
    eligible geometry under a tracked category node that is not tracked (tracking misses).
-   `draw parity OK|MISMATCH`: `DrawIndexed` arguments and the bound index and vertex buffers.
-   `render state … ->`: the cull, depth, stencil, blend and alpha-test state the native draw used, per
    property-derived key. A key marked `VARIES` means the state is not a function of the key.

Whiterun, `CS_DCLF_CAPTURE_PARITY=1`: about 1,300 objects in the tables; about 365 of them drawn natively
per frame; 0 mismatches in every check; 0 tracking misses.

Community Shaders fixes that came out of it: TruePBR wrote partly initialized constant arrays
(`TruePBR.cpp`, now value-initialized).

## Switches

| Variable | Effect |
| --- | --- |
| `CS_DCLF_STATS=1` | Every 300 frames, log how many objects are tracked, why the rest stay native, and the CPU time scene capture takes. |
| `CS_DCLF_CAPTURE_PARITY=1` | Compare the tables with the native draws (see above). Costs CPU on every draw. |

## Still open in Phase 1

-   `PerTechnique` constants (fog, output clamp) and filter modes, which `SetupTechnique` sets; it binds
    real shaders, so it cannot be evaluated with stand-ins and needs a port.
-   Community Shaders' own per-draw data: the permutation buffer (b4, `ExtraShaderDescriptor` bits such as
    additive lighting), and the feature resources `State::Draw` binds.
-   Coverage runs in an interior, a dungeon and across a cell transition.
-   Cost: rebuilding the tables takes about 1.0 ms of render-thread CPU per frame in Whiterun (peaks about
    1.5-2 ms), most of it the per-frame stand-in evaluations of 167 materials and 17 pipelines; tracking takes
    0.05 ms. Re-evaluating only what changed is the obvious next step.
