# P5.4: the water on the render graph

Plan task P5.4 ([org-full-port.md](../org-full-port.md), Phase 5): the BSWaterShader draw family drawn by ORG through Drawcall Limit
Fix, like the sky ([sky.md](sky.md)). Reverse engineered with GhidraMCP on SE 1.5.97 and AE 1.6.1170 (2026-11-10). IDs are Address
Library IDs. The code is `src/Features/DrawcallLimitFix/Features/Water/` and the switch is `CS_ORG_WATER=1` (default off).

## RE

### The shader

`BSWaterShader` (SE vtable `0x141865360`). Renamed in Ghidra (SE): `BSWaterShader::SetupTechnique`, `SetupMaterial`, `SetupGeometry`,
`BSWaterShaderProperty::GetRenderPasses`, `GetStencilPass`, `GetStencilPassVertexColor`. The AE renames did not take (the server
answers success and keeps the old names, as the sky lane found).

| Function | SE ID / addr | AE ID / addr |
| --- | --- | --- |
| SetupTechnique (slot 2) | 100601 / `0x1412f6000` | 107362 / `0x1414e0df0` |
| RestoreTechnique (slot 3) | `0x1412f5df0` (no Ghidra function) | |
| SetupMaterial (slot 4) | 100602 / `0x1412f63d0` | 107363 / `0x1414e11c0` |
| SetupGeometry (slot 6) | 100604 / `0x1412f6a70` | 107365 / `0x1414e1890` |
| RestoreGeometry (slot 7) | 100605 / `0x1412f8030` | |
| BSWaterShaderProperty::GetRenderPasses | 100238 / `0x1412d7a70` | 106945 / `0x1414c02e0` |
| GetRenderPasses for the local map (LOD water) | 100239 / `0x1412d7ea0`, 100244 / `0x1412d8240` | 106946 / `0x1414c08e0` |
| The stencil pass (no vertex colours / vertex colours) | 100242 / `0x1412d8060`, 100243 / `0x1412d8150` | |

**Pass ids** (SetupTechnique's switch, the same in AE):

| Pass id | Raw descriptor | Technique |
| --- | --- | --- |
| `0x5C00006B`, `0x5C00006C` | `0x5800`, `0x5801` (+ Vc) | Simple (11) |
| `0x5C00006D`, `0x5C00006F` | `0x5000` | Stencil (10) |
| `0x5C00006E`, `0x5C000070` | `0x5001` (+ Vc) | Stencil (10) |
| `0x5C000071 + d` | `d` | `d >> 11 & 0xF`: 0-7 Specular with that many lights, 8 Underwater, 9 LOD |

GetRenderPasses makes, per water shape: the surface pass (`0x5C000071 + (technique << 11 | flags)`, technique 0, 8 underwater or 9
LOD), the stencil pass (`0x5C00006D/6E`, or `6F/70` with accumulation hint 13 for a vertex-coloured property), and, when lights touch
it and it is not underwater, a lights pass (technique = light count - 1, at most 7, flags Vc, NormalTexCoord, Wading only).

**SetupTechnique** (RSS offsets from SE `0x143027eb0`): BeginTechnique, cull mode (+0x9C) none; a specular light technique (1-7) blend
mode 2 (SRC_ALPHA, ONE: the lights add); passes `0x5C00006B..70` write mode 9 (RT0 blue and RT1 RGB: the mask's normal and motion
vectors); the other passes write mode 1 and **RT0 = kMAIN, or kRAW_WATER (88) when `DAT_143233639`** (the water drawn for ISWaterBlend
to composite), or kLOCAL_MAP_SWAP in render mode 0x12. Then maps VS and PS b0 (PerTechnique: VPOSOffset, PosAdjust, CameraDataWater
with the camera's near and far from `DAT_1431d0e68+0x160/164`, SunDir, SunColor) and binds both at b0.

**SetupGeometry** maps VS and PS b2 (World, PreviousWorld, WorldViewProj, ObjectUV, CellTexCoordOffset; TextureProj, ReflectPlane,
ProjData, and per light pass NumLights, LightPos, LightColor from the pass's scene lights), and sets the **stencil**:

| Pass | Depth mode | Stencil mode, reference |
| --- | --- | --- |
| `0x5C00006F/70` (hint 13) | 1, bias 1 | technique 0: modes 32-38 (EQUAL on the water type's bit, INCR), ref 0xff; lights: 14-20 (EQUAL on the bit), ref 0xff |
| Stencil technique, not wading | 3 (write) | modes 2-9: write the water type's bit (`water+0x8c`, 2..0x80), ref = the bit |
| other techniques, not wading | 1 | mode 30 (NOT_EQUAL on bit 0), ref 1 |
| wading | 1, bias 1 | modes 14-20 with refs 3, 5, 9, 0x11, 0x21, 0x41, 0x81 |

Textures (PSTexture slots, RSS +0x140): t1 refraction (Refractions), t2 displacement (Wading: kWATER_DISPLACEMENT), t3 the cubemap
(Reflections: the property's or `DAT_14302c8f0`), t4-t6 the normal maps, t7 depth (Depth), t8/t9 the flow map and its normals, t10/t11
the SSR targets; Unified Water and the cubemap fix change some of them before the draw (they hook the same slot).

### The draws

The water is drawn at **two batches of the main camera's accumulator**, both through `BSShaderAccumulator::RenderBatches` and
`DrawTriShape` (R1) for the water's TriShapes:

| Batch | Call (SE / AE) | What |
| --- | --- | --- |
| The mask | `FinishAccumulating` 99938 `+0x4A6` / 106583 `+0x492` | `RenderBatches(0x5C00006D .. 0x5C000070)` after `ClearDepthStencil(STENCIL)`, clear colours 0, RT0 = `NORMAL_TAAMASK_SSRMASK` or its swap (the getter alternates by frame), RT1 = kMOTION_VECTOR, RT2-4 none. Draws the Stencil technique: the water's normal mask (blue), its motion vectors, and each water type's stencil bit. After DCLF's colour epoch and the blended decals. |
| The water | `FinishAccumulatingPostResolveDepth` 99939 `0x1412cb2e0` `+0x164` / 106584 `0x1414b3390` `+0x161` (renamed in SE) | `RenderBatches(0x5C000071 .. 0x5C006071)`: the surface, underwater, LOD and light passes, after the under-water blended draw (`+0x54`) and hint 10's group, before ISWaterBlend (`FUN_1412f05b0`, ImageSpaceManager +0x200, from kRAW_WATER into kMAIN when `DAT_143233639`) and DCLF's forward epoch (`+0x551`). |

FinishAccumulating also copies the main depth into kPOST_ZPREPASS_COPY again when water passes exist and flags bit 7 is set (SE
`0x1412cb07e`): the water's t7. FinishAccumulatingPostResolveDepth is vtable slot 0x2B (SE 99937 dispatches render mode 0 to it).

**Not BSWaterShader:** waterfalls and rapids are BSEffectShader meshes (DCLF's effect families); the underwater mask
(ISUnderwaterMask, Water.hlsl's UNDERWATERMASK) and ISWaterBlend are ImageSpace effects (P5.6).

**Main::RenderWaterEffects** (35561 / 36560) runs before the main pass: the water system's update (TESWaterReflections::Update 31383,
the displacement simulation through ImageSpaceEffectWaterDisplacement, 31385 and 99025) and the flow copies Unified Water patches out.

### The displacement simulation: left to the engine

ImageSpaceEffectWaterDisplacement (ImageSpace slot 31, shaders 86-94) runs the wading and rain ripple simulation into
kWATER_DISPLACEMENT and its swap. None of its nine shaders has a CS HLSL in the descriptor map (vanilla DXBC runs), the ImageSpace
chain is P5.6's (another lane), and only the wading water reads its output. So it stays the engine's: it runs earlier in DXVK's stream
than the EngineDraws epoch, and the water's draws read kWATER_DISPLACEMENT at t2 as the engine binds it, through GpuTextures.

## The family

The pieces every engine draw family shares are `Core/Engine/Family` (render-graph.md, "Shared helpers for engine families"): the
DrawTriShape taker, the SetupGeometry watch, the pass count, the geometry latch, the viewport, constant copies, slot textures, the
header resolver, the async programs, the kept leases and the run step.

- **The batches** (`WaterCapture.cpp`, thunks on both calls). For the main camera's accumulator with kMAIN's depth bound, the batch
  runs with its draws latched: every pass goes through BeginPass, SetupTechnique, SetupMaterial, SetupGeometry (Unified Water's, Light
  Limit Fix's, the cubemap fix's and the frame annotations' hooks with them) and render state as before.
- **Setup, withholding, running** (primary path, primary-path.md). The client is made at CS's setup (`Family::RunSetups`,
  before the setup's graph build), so it is in its anchor's graph before any world frame. A batch is taken when the client's targets match and `FrameAnchors::CanRun` says
  the anchor's run will draw it; otherwise the engine draws the batch, vouched for (`Flush::EngineDraws`), logged once. After the
  batch (or at a refusal) `Family::RunWithheld` runs the anchor; any status but `Ran` is an error, logged once, and nothing is
  retried. The client's `executed` callback counts the runs that drew.
- **The choke point** (the family's taker at the shared DrawTriShape detour). A latched draw records: the shadow state
  (`EngineDrawState::Latch`: depth, **stencil mode and reference**, blend, write mode, cull, bias), the six constant groups (VS and PS
  b0-b2) through **ConstantMirror** (watched from a SetupGeometry hook), Light Limit Fix's strict lights (PS b3, its CB mirrored),
  Deferred's directional shadow record (t98, mirrored: the CPU writes it), CS's permutation block (`State::permutationData`: the pixel
  stage branches on its descriptors), t0-t11 and their samplers (GpuTextures; an empty slot reads a null view of its declared kind, a cube at t3), the
  geometry (GpuResources leases, the index range and stride checked against the buffers, a triangle list only) and the viewport (less
  the bias mode's viewport offset, `Family::LatchViewport`).
- **The pass** (`WaterDraws.cpp`). After the batch the EngineDraws anchor runs (a new anchor, shared with the screen blood, three runs a
  frame). Each view has a client (`cs.water.mask`, `cs.water.surface`) whose pass draws the batch's draws in the engine's order, one
  RasterProgram draw each, with `Water.hlsl` built for ORG: the defines are ShaderCache's for the vertex descriptor (the pixel stage reads
  Wading at run time), the pipeline the engine's state objects' descriptions (`Pipeline::FindBlend` and the others), built off the
  render thread (`Family::EngineDrawPrograms`: a draw whose program is still building stays the engine's), and **`DrawDesc::stencilReference`** (new in
  RasterProgram) sets the draw's reference. Targets: the mask's two (kNORMAL_TAAMASK_SSRMASK, its swap, kMOTION_VECTOR imported once;
  each draw names the ones its shadow state holds), the surface's one (kMAIN or kRAW_WATER), and kMAIN's depth-stencil read and written.
- **Per-draw latch** (2656 bytes): the constant groups, descriptors, texture and sampler indices and geometry (`WaterOrgDraw`), the strict
  lights and the directional shadow record. The push block (112 bytes): the shared constants, three latch regions (the draw, the strict
  lights, t98) and 18 header handles.
- **The headers.** Water.hlsl's pixel stage includes Skylighting, the fog, Cloud Shadows, Dynamic Cubemaps, Light Limit Fix, IBL,
  Terrain and Volumetric Shadows, Wetness Effects and the shadow sampling header. Each resource is an `EXTERN` entry. What CS binds there
  at the batch decides how it is read: a **published** graph resource is read by its id's index, which the client's pass declares
  (its writes are ordered before the water; "Published inputs" below); a D3D11 texture is read by heap index (GpuTextures, resolved
  per batch, no graph declaration); nothing reads a null
  view of the header's declared kind (`GpuTextures::NullOf`: 2D, 2D array, 3D, cube or a buffer); a **D3D11 buffer** (Light Limit Fix's D3D11 local shadows) cannot be read, so the batch stays the engine's (logged once:
  turn on its feature's ORG path). The checks built every technique with every feature define (below), so no header resource is left
  register-bound.
- **Refusals end the latched part.** There is no probe: every batch is latched from the first. A draw the family cannot take (its
  inherent refusals, "What stays the engine's") runs the draws latched so far at that point and leaves the rest of the batch to the
  engine, vouched for (`Flush::EngineDraws` until the batch returns), so the engine's draws come after the graph's in the engine's
  order. A pass the dispatcher draws through another function than DrawTriShape
  (`Family::DrawsThroughTriShape`) is refused at its SetupGeometry, before the engine draws it. A batch that ran nothing
  keeps its imports (`Keep`).
- **Refusals** are logged once per reason, technique and view (`[DCLF] water: the engine draws: a surface draw of technique N: <reason>`,
  `Util::ReasonLog`; a texture that cannot be imported says why, `GpuTextures::DescribeFailure`):
  geometry, too many draws (64), not BSWaterShader, topology, a colour target the client does not import, constants not mirrored yet,
  state objects, the program, buffer or texture imports, a published input the client cannot read, samplers, another draw path.
- **What the mask reads.** The mask's pixel stage (the Stencil technique) samples no material slot and no header: it writes the normal
  mask from its positions' derivatives and the motion vectors. So its slots and headers are latched as null views, whatever the engine
  or CS left bound (`SlotRead`, `HeadersRead`). Before, a probe imported those stale bindings: streamed textures without their full
  window refused the mask, and published targets (`kHDR_DOWNSAMPLE4`, `kHDR_DOWNSAMPLE6`) changed its shape. The surface never samples
  t0 (ReflectionTex), which is latched the same way.

### Published material slots (history)

With the ImageSpace port and the engine targets on (every p5-all run), the refraction copy and the SSR targets the water samples at
its material slots are engine targets a graph pass writes, and EngineTargets publishes a target once a graph reader asks for it. A slot
holding a published export was refused (`a texture slot holds a published graph texture`), so the surface view (kMAIN and kRAW_WATER)
stayed the engine's in every p5-all run: the residue census's `RT kRAW_WATER DS kMAIN` and Water classes, 3-8 draws a frame
(`1111-072658-p35-p5-all-census-w3`).

Then (`org-port/w3-misc`) such a slot was read by its id (the id's whole-resource view index in the draw's latch), and the id was part of the view's shape, learnt by the probes before the client was made. That broke in the p5-all tour
(2026-11-11): both clients registered at 11:01:31, and the engine targets the surface samples were published after that, when
ImageSpace's port and the first SSR and cubemap uses asked for them (11:01:34 on). The next batches' shapes differed, and the surface
stayed the engine's for the session (`published inputs changed since its client registered (material slots +Engine::Cubemap::kREFLECTIONS,
+Engine::Depth::kPOST_ZPREPASS_COPY, +Engine::kSSR, +Engine::kSSR_RAW)`), as did the mask (`+Engine::kHDR_DOWNSAMPLE4,
+Engine::kHDR_DOWNSAMPLE6`, stale bindings it never samples).

### Published inputs

A client's published inputs no longer depend on what is published when its first batch comes. They are fixed when the client is made
(`Family::PublishedInputs`, `ListInputs` in `WaterDraws.cpp`): every id the view's draws can read.

- **The surface:** the ids DCLF's registry lists at the headers' registers (the ported features' frame textures and frame graph
  buffers: Light Limit Fix's lights and local shadows, the fog's volumes, Cloud Shadows, Dynamic Cubemaps, Skylighting, Terrain
  Shadows, IBL, Volumetric Shadows' copy, Terrain Blending's depth at t17), the engine depths CS binds at a header
  (`Engine::Depth::kPOST_ZPREPASS_COPY`, `kMAIN_COPY`, `kPRECIPITATION_OCCLUSION_MAP` at t70), and the engine targets the material
  slots hold (`Engine::kMAIN_COPY`, `kREFRACTION_NORMALS`, `kWATER_DISPLACEMENT` and its swap, `kWATER_FLOW`, `kSSR`, `kSSR_RAW`,
  `kSSR_BLURRED0`, `Engine::Cubemap::kREFLECTIONS`).
- **The mask:** none (it reads no slot and no header).

At each build of the EngineDraws graph the client's pass declares a read, for order only, of every listed id that every graph
registers (`PublishedResources::IsRegistered`), and records it as declared. An id stays registered once published, so the set only
grows, and it grows on an id's first Publish, which rebuilds every graph anyway: the client is never registered again for it. This is
the rule DCLF's frame textures follow (`Extensions::DeclareGraphTextures`, the commit 59a80e32 "one rule for graph textures").

A batch then chooses per run. A header or slot bound to a published export is read by its id's index
(`PublishedResources::ShaderResourceIndex`, in the run's header indices or the draw's latch) once a build declared the id; the record
re-records when a header's index changes (the run's revision). Until then, and for an id not listed, the batch stays the engine's,
logged once per id (`[DCLF] water: the engine draws: a surface batch whose t70 holds '<id>', ...` or `a surface draw's t3 holds
'<id>', ...`, with `no build of its client's graph has declared it yet` or `it is not among the view's published inputs`). A new id
in that log belongs in `ListInputs`. Before the client is in the built graph, probes accept any export: they only ask for programs
and constant copies.

The shape is now the view alone, so `Ready` no longer compares inputs and no `published inputs changed` line exists.

**Not the water's:** the census's Utility draws into `kREFRACTION_NORMALS` (vs `0x61b`) are Lighting objects' refraction pass, DCLF's
refraction class (flush.md, "DCLF's share").

### Graph rebuilds

- **Both clients are made at setup** (the main menu), with every published input listed; the engine targets among them are
  imported then, so the first build declares them.
- **Nothing registers again during play.** The published inputs are a fixed list (above): a newly published one joins the declared
  reads at the rebuild its first Publish causes for every graph, not through a new client. A header's input changes only a heap index
  in the recording (re-recorded, not rebuilt), and the targets are imported once (the mask's alternating normals target is two
  imports, chosen per draw).
- Toggling a ported feature's ORG path mid-session: a withdrawn id stays registered and declared, its export no longer matches the
  bound view, and the batch reads the D3D11 view by heap index.

### Accepted differences

- **A withheld batch whose run then fails** (an epoch failure, after `CanRun` passed) is not drawn that frame; the family stops at
  the failure.
- **Light Limit Fix's strict lights** are copied into each draw's latch from its CB (1232 bytes a draw).
- **The mask's and the water's batch order** against the engine's other work is the engine's: each runs where its batch was.
- **First-person water, the local map's water and reflections** are other accumulators: the engine's.

## The switch

`CS_ORG_WATER=1` (render-graph.md, "Switches"); default off. It needs DCLF running. `CS_ORG_WATER_PARTS=mask`, `surface` (or both,
comma separated; the default) keeps the other part on the engine, a fallback for a part the graph draws wrong. The bisect switches of
the first runs (`CS_ORG_WATER_SAFE`, `CS_ORG_WATER_TECHNIQUES`, the `CS_ORG_WATER_VIEWS` alias) are gone: their faults were found. The feature is registered in DCLF's list (`Features/FeatureList.cpp`, `Water::Feature()`, after Sky);
its status line is DCLF's `water` entry.

## Shader checks

`Water.hlsl`, `BloodSplatter.hlsl` and the headers they changed (ShadowSampling, Volumetric Shadows, Exponential Height Fog, Wetness
Effects, Light Limit Fix): `verify-shader-refactor.ps1` IDENTICAL for Water (vs, ps: the specular, light, underwater, LOD, stencil and
simple techniques with their flag sets, with and without every feature define), Sky, Lighting and Effect. `check-dual-target-shaders.py`
(FXC, SPIR-V, DXIL, spirv-val, push block 112 bytes) for 82 Water permutations with every feature define (`CLOUD_SHADOWS
DYNAMIC_CUBEMAPS ENABLESSR EXP_HEIGHT_FOG HORIZON_FIX IBL LIGHT_LIMIT_FIX SCREEN_SPACE_SHADOWS SKYLIGHTING TERRAIN_SHADOWS
UNIFIED_WATER VOLUMETRIC_SHADOWS WATER_EFFECTS WETNESS_EFFECTS TERRAIN_BLENDING EFFECTS11 ENABLE_LL ENABLE_ACESCG CS_GAME_GAMMA=2.2
TRUE_PBR GRASS_LIGHTING EXTENDED_MATERIALS SSS SCREEN_SPACE_GI POSTPROCESS RAIN_REFRACTION LOD_BLENDING TERRAIN_VARIATION
TERRAIN_HELPER CS_SKIN CS_HAIR`), each with no binding outside the heap in its DXIL. SIMPLE with REFLECTIONS or FLOWMAP and the light
techniques with REFRACTIONS fail on FXC before and after (the engine never builds them). The light techniques' pixel stage under
LIGHT_LIMIT_FIX reads nothing (no push block). Sky's ORG build still passes (its fog volumes take a zero handle).

## Test plan

1. Build, then `water-org0.env` and `water-org1.env` (`tools/cs-ab/scenarios/`): Riverwood at 13:00, the river by the mill from its south
   bank (clear, then rain with `fw c821f`, SkyrimOvercastRain in Skyrim.esm), a shot under the river (z -420; the water plane is at -250
   there), then Dragonsreach. Both set `CS_ORG_LIGHT_LIMIT_FIX_SHADOWS=1` (the water reads t102 only as a published buffer).
2. Log: `[DCLF] water: on (CS_ORG_WATER=1)`, `hooks installed`, `the cs.water.mask client registered (published inputs it may read:
   none)` and `cs.water.surface` (with its list), no `published inputs changed`, `[DCLF] water: built the program for descriptor ...`, `every mask water draw can be drawn by the graph` (and
   `surface`), `the first cs.water.surface run drawn by the graph`. No `back to the engine's draws`, no `a withheld batch was not
   drawn this frame`, no `the engine draws` after the first frames, no `SPIR-V build of Water.hlsl` errors, no `[E]` lines but the known EFFECTS11 one.
3. Shots against water-org0's: the river's colour, reflections (sky and banks), refraction and depth fade, the flow, rain ripples
   (kWATER_DISPLACEMENT, the engine's), the SSR mask (the water's blue in the normals target feeds SSR: reflections on the water's
   surface), the underwater shot (fog, underwater technique), and the shoreline (stencil bits: no water drawn over the banks).
4. The DXVK draw count of the forward part drops by the water's draws (the DCLF census counts the passes as registered).
5. If the underwater shot is above water, lower `z` in both scenarios.

Not verified without a build: everything above. Risks to look at first: Load<T> of the column-major matrices in the t98 latch, the
TextureProj transpose, the mask's write mode 9 (two targets with different
masks), and GpuTextures reads of engine render targets the graph writes in earlier epochs (kMAIN_COPY, the SSR targets).

## Primary path (2026-11-11, `org-port/primary-families`)

Programs (all four families, `Core/Engine/Family/EngineDrawProgram.{h,cpp}`): the family's ORG stages are compiled into the shader
store when CS loads each of its shader's descriptors (`ShaderCompilationTask` -> `DrawcallLimitFix::PrecompileShader` ->
`Family::PrecompileStages`; before the render graph has a compiler they wait for `Family::StartStagePrecompile` at setup). A program
key is a value: the descriptor CS looks the shader up by (`State::modified*Descriptor`), the variant, the D3D11 descriptions of the
blend, depth-stencil and rasterizer states field by field (`Family::DescribeStates`), the target formats. Every key drawn is written
to `Data/ShaderCache/v2/engine-draw-programs.txt`, and the next start queues the listed keys on ProgramCache's build thread at setup
(`EngineDrawPrograms::Prebuild`). A draw whose prebuilt key is still building waits for it (`the first draw waited N ms for its
prebuilt program`); a key never seen is built on the render thread at its first draw and logged once (`was not prebuilt: built on the
render thread at its first draw`). Constants come from the engine constants capture (`Family::CaptureConstants`,
`CS_ORG_ENGINE_CONSTANTS=1`, which the families now require), so a buffer's first use has its bytes.

### What stays the engine's

- A batch of another accumulator than the main camera's, a cube face, or with a depth target other than kMAIN's.
- A header that is a D3D11 buffer (Light Limit Fix on D3D11) or cannot be imported; a published header or slot not listed, or first
  published during play (an error).
- Per draw: not BSWaterShader, a colour target its view's client does not import, a state with no description or no ORG form,
  geometry that is not a static triangle-list TriShape within its buffers, a texture that cannot be imported, sampler modes with no
  graph sampler, more than 64 draws, an inactive engine constants capture.
- The water displacement simulation (ImageSpace's, P5.6) and the raw-water and refraction-normal passes DCLF does not claim.

Changed with the primary path: the program's descriptor is CS's lookup (`State::modifiedVertexDescriptor`, which drops the
Reflections, Cubemap and Interior bits as CS's own D3D11 shader does), so the startup enumeration and the draw agree. The directional
shadow record (t98) is read from Deferred's CPU data (`Deferred::GetDirectionalShadowLightData`), not from the bound buffer.

### What can trigger a graph rebuild (primary path)

The two clients' registrations at setup. Nothing in play registers again.

### What to look for in the log

- `[DCLF] <family>: the <id> client registered at setup (...)` once per client, at CS's setup, before `graphs built at CS's setup`, and no `frame anchors: cs.<family>.*
  registered` line after it; `; not published yet: <ids>` names inputs no producer had published by then.
- `[DCLF] engine draw programs: the manifest lists N keys`, `[DCLF] <family>: N programs from the manifest queued`, and on a second
  run no `was not prebuilt` line. Each such line names a key the enumeration or the manifest missed.
- `[DCLF] <family>: the engine draws: ...` lines name inherent refusals (once each); `[E]` `latched draws were not drawn this frame`
  and `the engine draws a batch: ... its anchor cannot run now` are errors.
- With the latch on, the census's `allowed engine draws` show `<family>: left to the engine` (and `sky: cloud layers`).
