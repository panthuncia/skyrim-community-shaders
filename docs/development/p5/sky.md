# P5.3: the sky on the render graph

Plan task P5.3 ([org-full-port.md](../org-full-port.md), Phase 5): the BSSkyShader draw family drawn by ORG through Drawcall Limit
Fix, in the main view and the water reflection's faces, with Cloud Shadows' fourth render target as an output of the ORG pass.
Reverse engineered with GhidraMCP on SE 1.5.97 and AE 1.6.1170 (2026-11-10). IDs are Address Library IDs. The code is
`src/Features/DrawcallLimitFix/Features/Sky/` and the switch is `CS_ORG_SKY=1` (default off).

## RE

### The shader

`BSSkyShader` (SE vtable `0x141865c70`). Its slots: 2 SetupTechnique, 3 RestoreTechnique, 4/5 the base (empty) SetupMaterial and
RestoreMaterial, 6 SetupGeometry, 7 RestoreGeometry. Renamed in Ghidra (SE): `BSSkyShader::SetupTechnique`, `SetupGeometry`,
`GetTechniqueIndex`. The AE renames did not take (the server answered success but the names did not change).

| Function | SE ID / addr | AE ID / addr |
| --- | --- | --- |
| SetupTechnique | 100639 / `0x1412fa900` | 107402 / `0x1414e5d00` (the technique index is inlined) |
| SetupGeometry | 100640 / `0x1412fabb0` | 107403 / `0x1414e6030` |
| GetTechniqueIndex (pass id to raw index) | `0x1412fb440` | inlined |
| RestoreTechnique | `0x1412fa810` (no Ghidra function; read from the bytes) | |

The pass ids and raw indices (`FUN_1412fb440`; CS's `ShaderCache::SkyShaderTechniques` is the raw index):

| Pass id | Raw | Technique | CS defines |
| --- | --- | --- | --- |
| `0x5C00005D` (any other) | 0 | SunOcclude | OCCLUSION |
| `0x5C006072` | 1 | SunGlare | TEX DITHER |
| `0x5C00005F` | 2 | MoonAndStarsMask | TEX MOONMASK |
| `0x5C000060` | 3 | Stars | HORIZFADE |
| `0x5C000062` | 4 | Clouds | TEX CLOUDS |
| `0x5C000063` | 5 | CloudsLerp | TEX CLOUDS TEXLERP |
| `0x5C000064` | 6 | CloudsFade | TEX CLOUDS TEXFADE |
| `0x5C000061` | 7 | Texture (sun, moons) | TEX |
| `0x5C00005E` | 8 | Sky (the dome) | DITHER |

**SetupTechnique** (shadow state offsets from RSS, SE `0x143027eb0`): `BSShader::BeginTechnique` with the technique's vertex and pixel
descriptors, then depth mode 1 (test, no write) for every technique, then per raw index: 0 write mode 0 (no colour); 1 depth off,
blend 2 (SRC_ALPHA, ONE), write mode 11; 2 alpha test on, write mode 0, depth mode 3 (write), depth bias mode 3 (the moon masks write
depth only, so the stars behind a moon fail); 3 blend 2; 4-7 blend 1 (SRC_ALPHA, INV_SRC_ALPHA), depth bias mode 2; 8 blend 1. Every
technique binds PS t2 = the noise gradient (`DAT_14302c928`), WRAP, POINT.

**RestoreTechnique** (`0x1412fa810`, decoded from the bytes): blend 0, write mode 1, depth mode 3, alpha test off, depth bias 0, then
`BSShader::EndTechnique`. So each technique starts from write mode 1 (RGB) and no bias.

**SetupGeometry** maps the vertex stage's PerGeometry (the current vertex shader's group 2, RSS+0x348 then +0x38) and the pixel
stage's (RSS+0x350 then +0x30), writes them through the shaders' constant tables, unmaps, and binds both at b2:

- `WorldViewProj`: the geometry's world (`+0x7c`, less the posAdjust; the first-person accumulator's offset when `acc+0x128`) times
  the camera's view-projection (RSS+0x430), transposed. `World` the same without the projection; `PreviousWorld` from the previous
  world (`+0xb0`) with the previous posAdjust.
- `EyePosition`: the accumulator's eye (`acc+0x16c`) less the posAdjust. `VParams`: the float `DAT_141e0df6c` (AE `DAT_1420330fc`).
- `TexCoordOff`: clouds only (sky object 3), the cloud layer's offset from `DAT_143233710[usCloudLayer]` (AE `DAT_14338cb10`).
- `BlendColor[3]`: zero, then `kBlendColor` (+0x88) for objects 0, 1, 3, 5, 6, 7; the atmosphere (2) takes the sky's three colours
  (`DAT_1432336d8`); the sky quad (4) zero with `[0].w = 1`.
- PS `PParams.x`: clouds' `fBlendValue` (+0xb8), `1 - x` for CloudsFade unless `bFadeSecondTexture`; `.y` the sky scale times
  ImageSpaceManager+0xe4, 0 for the sun glare and the moons.
- Textures: t0 the base (+0x98) or a default (`DAT_14302c900` for 0 and 3, `DAT_14302c920` for the others), WRAP, LINEAR (filter 1);
  CloudsLerp t1 = the blend texture (+0xa0); CloudsFade's t0 is the blend texture when `bFadeSecondTexture`.
- Blend 2 for the sun (0) and the moons (6).

Every offset and global is the same in AE (`DAT_14202aeb8`/`aec0` are the shader pointers, `DAT_14202afa0` the view-projection).

### The draws

The sky's passes register through `BSBatchRenderer::RegisterPass` like any other (push-front into the technique's group), and are
drawn by **`BSShaderAccumulator::FinishAccumulating`** (SE 99938 `0x1412cacd0`, AE 106583 `0x1414b2d90`), render mode 0's function
for every accumulator (FUN_1412cac20 dispatches by render mode), at one call:

| Call (SE / AE) | What |
| --- | --- |
| +0x8E / +0x84 | `RenderBatches(1 .. 0x5C00002F)`: the opaque batches (CS's `Main_RenderWorld_Start`) |
| ... | the opaque persistent lists (hints 9, 8, 1, 0) and techniques 0x30-0x5C |
| **+0x247 / +0x237** | **`RenderBatches(0x5C00005D .. 0x5C000064)`: the sky**, with alpha test on and its reference 128/255 |
| ... | write mode 11 |
| **+0x284 / +0x274** | **`RenderPersistentPassList(geometry group 13)`: the cloud layers** (SE `0x1412caf54` calls `0x141306240`, AE `0x1414b3004` calls `0x1414f19d0`, ID 100840 / 107630), when the group is a pass list (`+0x26 & 1`); else `RenderBatches(1 .. 0x5C006074, group 13)` |
| ... | write mode 1; decals (`BSShaderAccumulator::RenderDecals`, SE `0x1412cbb20`) |
| +0x319 / +0x308 | blended decals (`RenderBlendedDecals`, SE `0x1412cbcc0`); CS's `Main_RenderWorld_BlendedDecals` and DCLF's colour epoch are here |

**The cloud layers are not in the sky batch.** `BSSkyShaderProperty::GetRenderPasses` (AE `0x1414e6fd0`) gives a cloud pass (sky object
3, techniques `0x5C000062`-`64`) accumulation hint `0x11`; every other sky object gets none (the batch). The accumulator's registration
(AE `FUN_1414b2330`) puts hint `0x11` into geometry group 13 at its tail (`FUN_1414f17f0(group, pass, 0)`: appended, in registration
order) and hint `0x12` (BSEffectShader sky objects) at its head. So the clouds are group 13's pass list (`GeometryGroup::passList`,
linked by `BSRenderPass::passGroupNext`), drawn by `RenderPersistentPassList` right after the sky batch with write mode 11: each pass
through `RenderPassImmediately` (BeginPass, SetupGeometry, `DrawTriShape`), with the cull mode set per pass. This is why the probes of
round 2 and later never saw techniques 4-6 in the batch, and why the faces' Cloud Shadows overrides stayed on D3D11.

So the sky is drawn **after** the engine's opaque groups and **before** the decals and DCLF's colour epoch, into the GBuffer (CS's
deferred pass binds 8 targets; the sky writes the first three: `kMAIN`, `kMOTION_VECTOR`, `NORMALROUGHNESS`) with the Z-prepass copy
as its depth target. The sky draws at `z = w`, so it only covers pixels at the far plane; DCLF's opaque draws are on other pixels.

Inside the batch, `BSBatchRenderer::RenderTechniqueGroup` (SE `0x141308030`) sets the cull mode, alpha test and alpha-to-coverage per
group (0 and 4 back-face culled, 2 and 3 two-sided; 1, 3 and 4 alpha tested; group from `FindTechniqueGroup` `0x141308bb0`: two-sided
and alpha test bits, 4 for property flag bit 54), then `RenderPassImmediately` per pass: `BeginPass` (SetupTechnique), SetupMaterial,
SetupGeometry, and the geometry dispatcher's draw, **`DrawTriShape`** (SE 75477, AE 77263) for the sky's static TriShapes.

The reflection faces: `BSCubeMapCamera::RenderFaces` (100780 / 107564) clears the face (`kREFLECTIONS`, slice per face) and the
cube depth (`kCUBEMAP_REFLECTIONS`, slice 0, shared by every face) and submits the face's accumulator with render mode 0, which
reaches the same FinishAccumulating, so the same +0x247 call draws each face's sky after its opaque groups. DCLF's face draws come
later, in the depth epoch, loading the face (`reflection/view.faces`).

What is not in the batch: the **sun occlusion query** (SunOcclude) is drawn by `Sun::SubmitOcclusionQuery` (99958, `0x1403bbf20`)
from `Main::RenderWorld`'s end into the sun's own accumulator (Sun+0x50) with an engine occlusion query, read back by
`Sun::UpdateOcclusionQueries`; the **sun glare** (0x5C006072) is outside the range. Both stay the engine's.

**The depth bias viewport offsets** (`BSGraphics::CreateDepthBiasViewportOffsets`, SE 75829 `0x140d891f0`, AE `0x140e65d10`, renamed
in SE): the table is a constant, `{0, 1.5e-5, 0, 0, 0, 0, 2.6e-5, 1.5e-5, 2.8e-5, 1.7e-5, 3e-5, 1.9e-5}` (SE `0x141794530`, AE
`0x1419db270`). The sky's bias modes (0, 2, 3) take nothing off the viewport's depth range.

### Cloud Shadows on D3D11

Cloud Shadows hooks SetupGeometry (slot 6). For a cloud layer it finds the layer (`Sky::clouds`), and inside a face render it sets
`overrideSky`. At the draw's state flush (`State::Draw`, from `SetDirtyStates`) `SkyShaderHacks` then clears the face's first layer on
the face's first cloud draw, copies the previous layer into this one, binds the occlusion of last frame at t26, the face depth at t17
and its 4th target (the layer's face RTV), with its own blend state (SRC_ALPHA, INV_SRC_ALPHA on every target, no independent blend)
and no depth target. The last layer is copied into the occlusion cubemap at the next reflections prepass (t25, t26).

## The family

The engine's CPU code runs unchanged; its draw choke point latches state (Phase 5's second pattern).

- **The batch** (`SkyCapture.cpp`, a thunk on the +0x247 / +0x237 call). For the main camera's accumulator inside CS's deferred pass,
  or a face of `kREFLECTIONS`, the batch runs with its draws latched: every sky pass goes through the engine's BeginPass,
  SetupTechnique, SetupGeometry (Cloud Shadows' and the frame annotations' hooks with it) and render state as before.
- **The cloud group** (a thunk on the +0x284 / +0x274 call). When group 13 is a pass list of BSSkyShader TriShapes only, the capture
  spans it: the batch's capture stays open, the cloud passes are latched the same way, and the run is handed to the anchor after the
  group. A group with another shader's pass (a sky-object effect, hint `0x12`), another geometry type, or no pass list is drawn by
  the engine after the anchor's run, in the engine's order (logged once: `the main cloud layers stay the engine's while ...`). A
  probe whose only refusals were cloud draws leaves that view's clouds to the engine for the session (`the {main,face} cloud layers
  stay the engine's for the session`), so a cloud technique the graph cannot draw never keeps the dome off the graph.
- **The choke point** (a detour on `DrawTriShape`). While a batch is latched, a draw is recorded instead of drawn: the shadow state
  SetDirtyStates would apply (`EngineDrawState::Latch`: depth, blend, write mode, cull, bias, alpha-to-coverage), the alpha test
  reference, CS's extra descriptor (`IsSun`, `IsMoon`, `InReflection`), the PerGeometry constants SetupGeometry wrote (read through
  **ConstantMirror**: `Watch` from a SetupGeometry hook, `Contents` at the draw; no new decode was needed, the bytes are Sky.hlsl's
  layout), the technique's textures and samplers (`GpuTextures`), the static vertex and index buffers (`GpuResources::Frame` leases,
  by device address) and the viewport. Cloud Shadows' `overrideSky` and `bindLayerSelfShadow` are taken and cleared, so its D3D11
  overrides never apply to another draw.
- **The pass** (`SkyDraws.cpp`). After the batch (or the cloud group) the anchor runs: `Sky` for the main pass, `SkyReflections` for a face (new anchors
  at this call, in FrameAnchors). Its client's pass draws the batch's draws in the engine's order, one RasterProgram draw each, with
  CS's `Sky.hlsl` built for ORG: the defines are ShaderCache's for the pixel descriptor (`GetCompileDefines`), and the pipeline is
  the engine's state objects for the draw (`EngineBlendState` and the others, through `StateConversion`). The vertex stage pulls the
  geometry (`DCLFVertexPull.hlsli`); the constants are a latch region per draw; the shared constants come from `SharedConstants`.
  - Main: the three GBuffer targets the shadow state names, the depth target (the Z-prepass copy) read and written (the moon masks
    write depth), t17 the scene depth CS binds (Terrain Blending's published depth, or an engine depth; `kMAIN`'s when it is the
    depth target itself, as `VanillaBinding::BindSkyResources` does).
  - Faces: two passes. The draws that write no occlusion, on the face (the cube's slice) with the cube depth. Then the cloud draws
    Cloud Shadows overrides, on the face and its occlusion target (`SKY_ORG_FACES`: SV_Target1), without a depth target and with
    the cube depth at t17, with Cloud Shadows' blend state; the face's first such draw clears its occlusion slice.
  - Order: the cloud group follows the batch, so every cloud draw comes after the dome, stars, moon masks, sun and moons, in the
    group's registration order.
  - Every draw's vertex count is a run gate entry (`cs.sky.main.gate`, `cs.sky.faces.gate`) the client's commit opens for its run
    only, so a recording replayed for a run that did not commit (another client keeping the anchor's run) draws nothing. The Sky and
    SkyReflections anchors have no other client today.
- **Cloud Shadows' 4th MRT.** The faces' occlusion is Cloud Shadows' graph faces, `CloudShadows::Faces` (the layers' size and mips,
  R8, a 6-slice array, History, published), which `ORGCloudShadows::Setup` makes when the family asked for it at its install
  (`RequestGraphFaces`, unless `CS_ORG_SKY_VIEWS=main` or `CS_ORG_SKY_CLOUDS=0`). The face's cloud draws blend into its slice in
  order (the first clears it), which is what the last D3D11 layer holds.
- **The copy's source is fixed for the session.** Cloud Shadows' copy declares both sources from its first build, the D3D11 last
  layer and the graph faces, with a gated dispatch per source, face and mip, and its commit opens, for each face, the source that drew
  that face's clouds last: `ORGCloudShadows::NoteFace(face, Graph)` when a face run the graph draws holds every cloud draw, `Layer`
  from Cloud Shadows' `SkyShaderHacks` when the engine draws a face's clouds (a probe, a fallback, the family off), and every face
  `Layer` when `ReflectionsPrepass` clears the last layer for a weather without clouds. A change of writer is data, not a graph
  change: nothing rebuilds. (Wave 1's copy re-declared its source per change, from the faces' batches, and rebuilt the graph a few
  hundred times a minute until the device ran out of memory.) t25 and t26 keep their one-frame delay, and DCLF's frame texture and the
  height fog read the published occlusion as before.
- **Recording.** A pass's revision is the batch's draws without their constants (programs, buffers, textures, counts, face, viewport);
  the constants are the latch. A face batch has another revision per face, so the faces' pass is recorded again per face.
- **Probes.** A view's batches are the graph's only after a probe: a batch the engine draws while each draw is checked as the
  family would take it (its program built, its constants mirrored, its textures and buffers imported; Cloud Shadows' flags left for
  the engine). Once a probe latched every draw, the view's next batches are the graph's. A draw a later batch cannot latch makes the
  rest of that batch the engine's (the draws latched before it are drawn after the batch: one batch out of order) and the view
  probes again. Nothing is withheld for a program that does not build.
- **Refusals.** A draw the family cannot take is logged once per reason, technique and view (`a main sky draw of technique N stays
  the engine's: <reason>`): no TriShape buffers, too many draws, not BSSkyShader, a stencil state, no mirrored vertex constants, no
  state object, no program, a buffer or texture import, a sampler. A probe checks every draw (so one probe builds every program); a
  batch the graph draws stops at the first refusal. Every pass of a batch must draw through `DrawTriShape` (SetupGeometry calls
  against DrawTriShape calls): a pass that draws through another of R1's draw functions is the engine's at its place in the batch,
  ahead of the graph's draws, so the batch is refused and the log names the geometry types (`drew without DrawTriShape`).
  The cloud layers never reached the batch at all: they are group 13 (above), now spanned by the capture.
- **Constants.** A technique's PerGeometry group holds only the rows its vanilla shader declares; the latch takes what there is
  (at least c0-c12) and zeroes the rest, and a pixel stage without a group gets zero PParams.
- **The scene depth.** When CS's t17 is neither an engine depth nor a published texture (Terrain Blending's D3D11 depth), its view
  is imported (`Sky::SceneDepth`). Round 2 bound nothing there, so the sun's depth test read zero and hid the sun.
- **The headers' textures.** Exponential Height Fog's header includes IBL's (t76, t77) and Dynamic Cubemaps' (t30, t31) headers;
  the reflection fog reads t31. They are `EXTERN` entries of the ORG build (Dynamic Cubemaps' are header SRVs now), bound to what
  CS binds there for the pass: a published texture by id, or the D3D11 view imported (`Sky::t<register>`).
- **Fallbacks.** The faces' clouds stay the engine's (with Cloud Shadows' D3D11 override) when Cloud Shadows has no graph faces.
  A batch whose view or targets are not one of these, or whose client is new (registered for the new targets, then
  built and prepared by an empty run), is drawn by the engine. A draw that cannot be latched (a buffer not mirrored yet: the first
  batch after start; a texture or buffer import failing; a stencil state; a program that does not build; more than 128 draws) is drawn
  by the engine where it was. A failed graph returns every batch to the engine; Cloud Shadows' copy then reads each face from its D3D11
  layer once the engine has drawn that face's clouds again.

### Accepted differences

- **The main view's cloud self shadowing** (Effects11's cloud scattering, t26) reads the full occlusion of last frame's faces, where
  D3D11 binds the layers up to the drawn layer of this frame's faces. Per-layer occlusion cubes would need cube views of a cube
  array's slices, which ORG makes as cube-array views.
- **A face's cloud layer Cloud Shadows does not find** (not in `Sky::clouds`) is drawn with the face's other draws, before the
  overridden cloud layers.
- **Pipeline builds block** the render thread the first time a technique and state meet, as CS's D3D11 shaders compile on first use.
- **Up to seven anchor runs a frame** (the main pass and six faces), each an epoch.
- **A face batch that fails partway** leaves its occlusion split between the graph faces (the clouds latched before the refusal) and
  the D3D11 layers (the rest); the copy reads that face from its D3D11 layer for that frame.
- **The main view's clouds drawn by the engine** while the faces' are the graph's bind the D3D11 layer of their own index at t26,
  which the graph's faces no longer write: their self shadowing reads that layer's last D3D11 contents.
- **The moon masks' depth** is written into the Z-prepass copy as on D3D11, by the anchor's pass at the same point.
- **The sun occlusion query and the sun glare** stay the engine's (above).

## The switch

`CS_ORG_SKY=1` (render-graph.md, "Switches"); default off. It needs DCLF running and, while Cloud Shadows is loaded,
`CS_ORG_CLOUD_SHADOWS=1` (else the sky stays the engine's, logged once). Diagnostics: `CS_ORG_SKY_VIEWS=main` or `faces` takes only
that view; `CS_ORG_SKY_CLOUDS=0` leaves the cloud layers to the engine in both views. The feature is registered in DCLF's list
(`Features/FeatureList.cpp`, `Sky::Feature()`, before Fades); its status line is DCLF's `sky` entry.

## Test plan

1. Build, then `sky-org0.env` and `sky-org1.env` (`tools/cs-ab/scenarios/`): Whiterun at 13:00 and 22:00 in cloudy weather (12f2d),
   a shot up at the sky (`player.setangle x -75`) and at the horizon each time, then Dragonsreach. Then `sky-river-org0.env` and
   `sky-river-org1.env`: Riverwood's river in the same weather, a shot down at the water (the faces' sky and clouds in its
   reflection) and one up at the sky at each hour.
2. Log: `[DCLF] sky: on (CS_ORG_SKY=1)`, `hooks installed (the sky batch, the cloud group, ...)`, `[ORG] Cloud Shadows: the occlusion
   copy runs on the render graph (...); each face from its last writer, the D3D11 layer or CloudShadows::Faces`, `the cs.sky.main
   client registered (..., cloud layers the graph's)`, `the cs.sky.faces client registered` (with water in view), `built the
   program for technique N` for 4, 5 or 6 (`faces, cloud occlusion` for the faces), `every main sky draw can be drawn by the graph`
   (and `face`). The faces' warning `a face batch on the graph has no cloud draw` is gone. No `cloud layers stay the engine's`
   (if one appears, its reason says why), no `back to the engine's draws`, no repeated `could not be drawn by the graph`, no
   `SPIR-V build of Sky.hlsl` errors, no `[E]` lines but the known EFFECTS11 one.
   The graph must not rebuild during play: after the clients' first registrations no `frame anchors: cs.sky.* registered` line
   appears again (a weather or time change adds none), the device does not run out of memory, and the `Persistent preparation
   cost` lines keep listing `cs.sky.faces.clouds`.
   The ORG build was checked with every feature define CS has (check-dual-target-shaders.py, ps and vs, every technique, DEFERRED
   and SKY_ORG_FACES: FXC, SPIR-V, DXIL, no binding outside the heap); defines with values (feature options) were not.
3. Shots: `sky-noon`, `sky-night` (the dome, clouds, sun, moons, stars behind the moon masks) and the horizon shots against
   sky-org0's; `river-noon`, `river-night` (the water's reflection of the sky and its clouds) against sky-river-org0's; terrain cloud
   shadows in every shot (they read the faces' occlusion, now the graph's).
4. The DCLF census (`CS_DCLF_CENSUS=1`) still counts the sky's passes as registered (they are, and the engine's batch runs); the
   DXVK draw count of the GBuffer pass drops by the sky's draws.
5. With `CS_ORG_ANCHOR_SELFTEST=1` and `CS_ORG_SKY=1` the self-test reports Sky and SkyReflections OK.

Not verified without a build: everything above, and the cloud group's RE (from the decompiles: that every cloud pass is group 13's
and a TriShape, and that the faces' accumulators register them there too). Risks to look at first: the vertex pulling (index alignment, the VertexDesc key),
the faces' per-slice RTVs of the 6-slice array, `GpuResources::Frame()` leases outside tree LOD's BeginFrame, and the face depth
read as an SRV in the cloud pass after the other pass wrote it.
