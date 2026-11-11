# P5.3: the sky on the render graph

Plan task P5.3 ([org-full-port.md](../org-full-port.md), Phase 5): the BSSkyShader draw family drawn by ORG through Drawcall Limit
Fix, in the main view and the water reflection's faces, with Cloud Shadows' fourth render target (its layers) an output of the ORG pass.
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

The engine's CPU code runs unchanged; its draw choke point latches state (Phase 5's second pattern). The pieces every engine draw
family shares (the DrawTriShape taker, the SetupGeometry watch, the pass count, the geometry, viewport, constant, texture and header
reads, the async programs, the kept leases, the run step) are `Core/Engine/Family` (render-graph.md, "Shared helpers for engine
families").

- **The batch** (`SkyCapture.cpp`, a thunk on the +0x247 / +0x237 call). For the main camera's accumulator inside CS's deferred pass,
  or a face of `kREFLECTIONS`, the batch runs with its draws latched: every sky pass goes through the engine's BeginPass,
  SetupTechnique, SetupGeometry (Cloud Shadows' and the frame annotations' hooks with it) and render state as before.
- **Joining, withholding, running.** A view's client is made once, for the first batch's shape, and joins with
  `FrameAnchors::RequestJoin`: until the anchor's built graph holds it, the engine draws. A batch is withheld only when
  `FrameAnchors::CanRun` says the anchor's run will draw it (a probe withholds nothing). After the batch, `Family::RunWithheld` runs the
  anchor; any status but `Ran` is logged once per status (`a withheld batch was not drawn this frame`) and the view probes again.
  The client's `executed` callback counts the runs that drew (the status line's `drawn`).
- **The cloud group** (a thunk on the +0x284 / +0x274 call), with `CS_ORG_SKY_CLOUDS`. When group 13 is a pass list of BSSkyShader
  TriShapes only, the capture spans it: the batch's capture stays open, the cloud passes are latched the same way, and the run is
  handed to the anchor after the group. FinishAccumulating calls the group exactly when the capture spans it (non-null, flag 1). If
  the call does not come (another hook at its site), the capture is finished at the next sky batch and no batch spans the group again
  for the session (`the cloud group was not drawn after a sky batch that spanned it`). A group with another shader's pass (a
  sky-object effect, hint `0x12`), another geometry type, or no pass list is drawn by the engine after the anchor's run, in the
  engine's order (logged once). A probe whose only refusals were cloud draws leaves that view's clouds to the engine for the session.
- **The choke point** (the family's taker at the shared `DrawTriShape` detour). While a batch is latched, a draw is recorded instead
  of drawn: the shadow state SetDirtyStates would apply (`EngineDrawState::Latch`), the alpha test reference, CS's extra descriptor,
  the PerGeometry constants SetupGeometry wrote (ConstantMirror), the technique's textures and samplers, the static vertex and index
  buffers (`GpuResources::Frame` leases, by device address, their index range and stride checked against the buffers, a triangle
  list only: `Family::LatchGeometry`) and the viewport (`Family::LatchViewport`, with the depth bias table; the sky's modes take
  nothing off). Cloud Shadows' `overrideSky` and `bindLayerSelfShadow` are taken and cleared.
- **The latch.** Everything per draw is in the draw's latch region (`SkyOrgDraw`, 352 bytes): the constants, the geometry, the heap
  indices of t0-t2 and their samplers, and t26. The push block (52 bytes) holds only what a client's pass declares (the shared
  constants, t17, t25, the headers) and the latch handle, so a recording is kept while textures, buffers or constants change.
- **The pass** (`SkyDraws.cpp`). The anchor (`Sky` for the main pass, `SkyReflections` for a face) draws the batch's draws in the
  engine's order, one RasterProgram draw each, with CS's `Sky.hlsl` built for ORG: the defines are ShaderCache's for the pixel
  descriptor, the pipeline the engine's state objects' descriptions (`Pipeline::FindBlend`, `FindDepthStencil`, `FindRaster`),
  built off the render thread (`Family::EngineDrawPrograms` over `ProgramCache`): a draw whose program is still building stays the
  engine's. The vertex stage pulls the geometry (`DCLFVertexPull.hlsli`).
  - Main: the three GBuffer targets, the depth target read and written (the moon masks write depth), t17 the scene depth CS binds.
  - Faces: two passes. The draws that write no occlusion, on the face with the cube depth. Then the cloud draws Cloud Shadows
    overrides, on the face and their Cloud Shadows layer (below), without a depth target and with the cube depth at t17.
  - Every draw's index count is a run gate entry the client's commit opens for its run only.
- **Cloud Shadows' layers.** Cloud Shadows keeps per-layer occlusion: layer L of a face holds the occlusion of the face's layers up to
  L (SkyShaderHacks copies the layer below into it before the layer's draw). The main pass's clouds read their own layer at t26 for
  their self shadowing, and Cloud Shadows' copy reads the last layer into the occlusion cubemap. A face's cloud draw on the graph writes
  its layer through an import of it (`ORGCloudShadows::ImportLayer`, the D3D11 texture with its SRV and RTVs; the copy imports the
  last layer the same way, so one graph resource serves both). The copy from the layer below cannot be made at the latch (the layer
  below is drawn by the graph after the batch), so the layer starts cleared (`CloudShadows::NoteGraphCloudLayer`, on D3D11 at the
  latch, with the face's bookkeeping) and each cloud draw also draws its occlusion into the face's later layers (its face target
  write-masked, `ProgramKey::maskedTargets`). A layer then holds the occlusion of every layer up to it, on both paths; the cloud
  layers must come in rising order (a batch where they do not is the engine's). No clear runs outside the gate: a replayed recording
  only loads.
- **Self shadowing (t26).** A main pass cloud layer reads its own layer (`texCloudShadowLayers[currentLayerForDraw]`, by heap
  index), as SkyShaderHacks binds it; everything else reads the occlusion cubemap (t25's, `SKY_ORG_OCCLUSION`), as the face clouds do
  on D3D11. Binding the whole occlusion to the main clouds darkened every layer by all of them: the overcast look of the graph's clouds.
- **Probes.** A view's batches are the graph's only after a probe: a batch the engine draws while each draw is checked as the
  family would take it. Once a probe latched every draw, the view's next batches are the graph's. A draw a later batch cannot latch
  makes the rest of that batch the engine's and the view probes again. A probe keeps its imports (`SkyDraws::Keep`).
- **Refusals.** A draw the family cannot take is logged once per reason, technique and view (`[DCLF] sky: the engine draws: a main draw
  of technique N: <reason>`, `Util::ReasonLog`). Every pass of a batch must draw through `DrawTriShape` (`Family::Passes`).
- **Inputs.** t17, the headers (t76, t77, t30, t31) and t25 resolve through `Family::ResolveHeader`: a published graph texture is
  declared by the client's pass and is part of the client's shape; a D3D11 view (Terrain Blending's D3D11 depth, a header CS binds as a
  D3D11 view) is read by heap index, resolved per batch; an empty register reads a null view of its declared kind
  (`GpuTextures::NullOf`: a cube for t25, t30 and t31). A 2D null read as a cube is undefined on the GPU; AMD loses the device on it.
- **Fallbacks.** No client is registered again during play: a batch whose targets or published inputs differ from its client's
  shape stays the engine's (logged once). A draw that cannot be latched is drawn by the engine where it was. A failed graph returns
  every batch to the engine.

### What can trigger a graph rebuild

The two clients' first registrations (one per view, at the first batch of each). Nothing in play registers, refreshes or imports
again: the shape holds only published ids, D3D11 inputs are heap indices in the latch or the run, and the layer imports are made once
with the faces' client.

### Accepted differences

- **A face's cloud layer Cloud Shadows does not find** (not in `Sky::clouds`) is drawn with the face's other draws, before the
  overridden cloud layers.
- **Up to seven anchor runs a frame** (the main pass and six faces), each an epoch.
- **A face batch that fails partway** draws the rest of its clouds on the engine before the graph's: their layers' copies read the
  layer below before the graph has drawn it, for that frame.
- **A withheld batch whose run then fails** (an epoch failure, after `CanRun` passed) is not drawn that frame; the family stops at
  the failure.
- **The face clouds' layers are drawn again** into every later layer: n(n-1)/2 extra occlusion-only draws a face for n layers.
- **The moon masks' depth** is written into the Z-prepass copy as on D3D11, by the anchor's pass at the same point.
- **The sun occlusion query and the sun glare** stay the engine's (above).

## Cloud layers

`CS_ORG_SKY_CLOUDS` (`1`, `main` or `faces`) takes the cloud layers on the graph; unset, the engine draws them after the anchor's
run while the dome, stars, sun and moons stay the graph's. It stays off by default until the scenarios below show the graph's clouds
match: the two causes found (t26 bound to the whole occlusion for the main clouds; the faces' graph occlusion leaving the D3D11 layers
stale for the engine's main clouds) are fixed, and the faces now keep Cloud Shadows' layers as the engine does, but no run has
checked it yet. Found and fixed earlier: the main view's programs were built without DEFERRED (CS looks the sky's shaders up with
`ModifyShaderLookup`, which adds 256 in the deferred pass).

## The switch

`CS_ORG_SKY=1` (render-graph.md, "Switches"); default off. It needs DCLF running and, while Cloud Shadows is loaded,
`CS_ORG_CLOUD_SHADOWS=1` (else the sky stays the engine's, logged once). `CS_ORG_SKY_CLOUDS` is the feature choice above. The feature
is registered in DCLF's list (`Features/FeatureList.cpp`, `Sky::Feature()`, before Fades); its status line is DCLF's `sky` entry
(`runs (drawn), draws, programs`).

## Test plan

1. Build, then `sky-org0.env` and `sky-org1.env` (`tools/cs-ab/scenarios/`): Whiterun at 13:00 and 22:00 in cloudy weather (12f2d),
   a shot up at the sky and at the horizon each time, then Dragonsreach. Then `sky-river-org0.env`, `sky-river-org1.env` and
   `sky-river-org1-clouds.env` (`-clouds-main`, `-clouds-faces`): Riverwood's river in the same weather.
2. Log: `[DCLF] sky: on (CS_ORG_SKY=1) ... cloud layers: main ..., faces ...`, `takes its draws at the engine draw families'
   DrawTriShape detour`, `the cs.sky.main client registered`, `the cs.sky.faces client registered` (with water in view), `[DCLF] sky:
   built the program for descriptor ...`, `every main sky draw can be drawn by the graph` (and `face`), `the first cs.sky.main run drawn
   by the graph`. No `back to the engine's draws`, no `a withheld batch was not drawn this frame`, no `[E]` lines but the known
   EFFECTS11 one. After the clients' first registrations no `frame anchors: cs.sky.* registered` line appears again.
3. Shots against sky-org0's and sky-river-org0's: the dome, clouds, sun, moons, stars; the water's reflection of the sky; with the
   clouds on, cloud brightness and terrain cloud shadows (t25 from the last layer).
4. With `CS_ORG_ANCHOR_SELFTEST=1` and `CS_ORG_SKY=1` the self-test reports Sky and SkyReflections OK.

Not verified without a build: everything above. Risks to look at first: the latch's layout (`SkyOrgDraw` at 352 bytes, `Load<T>`),
the layer imports declared as render targets (192 face views) in the faces' cloud pass, the main clouds reading a layer the faces'
epoch wrote through a heap index, and the D3D11 clear of a layer at the latch before the anchor's run.
