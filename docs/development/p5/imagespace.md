# P5.6: the ImageSpace chain on the render graph

Plan task P5.6 ([org-full-port.md](../org-full-port.md), Phase 5). The engine's ImageSpace effects drawn by ORG passes in the
engine's place, with the CS HLSL of [imagespace-shaders.md](./imagespace-shaders.md) built for ORG and the constants of
[constants.md](./constants.md). R4 of [engine-d3d11-surface.md](../engine-d3d11-surface.md) is the effect list; this file adds
what R4 left open, designs the whole chain effect by effect, and describes the part that is built.

Code: `src/RenderGraph/Engine/ImageSpace/` (`ImageSpace.{h,cpp}`). Switch: `CS_ORG_IMAGESPACE=1` (default off), with
`CS_ORG_ENGINE_CONSTANTS=1`.

## What the RE settles (SE 1.5.97, AE 1.6.1170, 2026-11-10)

| Function | SE ID / address | AE ID / address | Notes |
| --- | --- | --- | --- |
| `BSImagespaceShader::Render` | 100950 / `0x14130cd20` | 107732 / `0x1414f88b0` | Slot 1 of the ImageSpaceEffect vtable (at +0x90 of every BSImagespaceShader), shared by every raster ImageSpace shader checked (ISCopy, the ISHDR family, the bright-pass blurs, the VL composites, the tonemap). Renamed in SE. |
| `ImageSpaceEffectHDR::Render` | 100465 / `0x1412e6fe0` | 107183 / `0x1414d0cb0` | Slot 1 of HDR's vtable (SE `0x141861e90`, AE `0x141abb468`). Renamed in SE; the AE rename did not take (as noted for the sky). |
| `ImageSpaceManager::RenderPreTonemapComposites` | 99024 / `0x141297910` | 105675 / `0x141481f60` | R4. |
| `BSGraphics::Renderer::DrawTriShape` | 75477 / `0x140d6bfe0` | 77263 / `0x140df7310` | The quad's draw: sets the vertex description and topology, `SetDirtyStates(false)`, IA binds, `DrawIndexed`. |

**How a raster effect draws** (`BSImagespaceShader::Render`, the same on AE):

1. The output is the effect's texture 0 (`ImageSpaceTexture::GetRenderTarget` of `effect+0x48[0]`), bound as render target 0 with
   mode 3 (`SRTM_RESTORE`: no clear); the depth-stencil target is none (-1). When the param's clear flag (+0xc) is set the
   renderer's clear colour is set, which only matters for a clear mode.
2. The viewport is the whole target (`Renderer::UpdateViewPort(0, 0, true)`) unless the effect's custom-viewport flag
   (`ImageSpaceEffect+0x88`) is set.
3. When the manager's rectangle (`ImageSpaceManager+0x10`, `+0x14`, `+0x18`, `+0x1c`) is set (not -1), a viewport of that
   rectangle is applied through 75564 (CS's LegacyGraphicsCompatibility adapter patches that call) and a shadow-state flag
   (0x1000). Menu 3D sets it.
4. `MakeRenderPass(shader, nullptr, quad, 0x5c006076)` and `BSBatchRenderer::RenderPassImmediately`: SetupTechnique (the PS/VS),
   SetupMaterial (textures and samplers into the shadow state's PS slots, from the effect's texture list: 0 is the output, 1 and
   up the inputs), SetupGeometry (GeometryOffset and the effect's constants through the custom mapper, VS and PS b2), and the
   quad's `DrawTriShape`.
5. The quad (`ImageSpaceManager+0x48`, built by `0x14129d650`; `+0x50` when `+0x58` selects the dynamic-resolution quad) is four
   vertices with UV (0,0) at the top left to (1,1) at the bottom right, so the graph's fullscreen triangle gives the same
   TEXCOORD0.

**Composites** (`ImageSpaceEffect::Render`, `0x1412fc000`) call each child's Render, then `SetDirtyStates(false)` without a
draw. Effects do not loop on their own; the HDR effect's luminance chain is an explicit loop (below). Nothing nests a second
chain.

**The HDR effect's Render** (slot 30), with target indices (CommonLib `RENDER_TARGET`):

| Step | Shader (slot) | Output | Inputs (t#) | Notes |
| --- | --- | --- | --- | --- |
| 1 | DownSample4 (58) | `kHDR_DOWNSAMPLE0` (26) | t0 = the effect's source | BlurOffsets for a 4-tap box, `1/size` of the source in c1 |
| 2 | 4RGB2Lum (60), then 16Lum (59) per level | `kHDR_DOWNSAMPLE1` (27) and up | t0 = the previous level | Each level a quarter the size, down to 1x1 (2x2 with `DAT_141e0f2f0`). The last pair alternates by frame (`DAT_143233530`): +2 on odd frames. |
| 3 | 16LightAdapt (64), when an earlier value exists | the next level | t0 = the last level, t1 = last frame's value (`DAT_141e0eb48`) | The result becomes `DAT_141e0eb48` |
| 4 | BrightPassBlur composite (slot 23 + clamp(radius - 1, 0, 6)), H then V | `kHDR_BLURSWAP` (22), then `kHDR_BLOOM` (24) | t0 = downsample0 / the swap, t1 = the luminance | Two draws of the same shader (79 + r) |
| 5 | Tonemap 55, or 56 (Fade) when manager+0x118 > 0 | the effect's output | bloom, the source, the luminance | CS's tonemap hook, Effects11 and Post Processing own this draw |

So a frame's HDR chain has a fixed set of shapes for a resolution, with a two-frame alternation at the luminance chain's end.

## The chain, effect by effect

"Where" is the engine call that draws it; "ORG" is what replaces it and when. Built = this task's subset, behind
`CS_ORG_IMAGESPACE`. Composites are not passes: their children are. Every pass is drawn at the `ImageSpace` anchor through the
mechanism below unless the row says otherwise.

| Effect (slots) | Where | Inputs | Outputs | ORG |
| --- | --- | --- | --- | --- |
| HDR downsample, luminance, adaptation (57-64) | HDR Render, inside 99023's tonemap call | source, levels, `DAT_141e0eb48` | `kHDR_DOWNSAMPLE0..13` | **Built** (ISHDR) |
| Bright-pass bloom (23-29: 79-85, then 65-71) | HDR Render | downsample0, swap, luminance | `kHDR_BLURSWAP`, `kHDR_BLOOM` | **Built** (ISBlur): the bright-pass draw into the swap and, since 5.5.4, the plain blur's draw into `kHDR_BLOOM` |
| Tonemap (55, 56) | HDR Render | bloom, source, luminance | the HDR output (`kIMAGESPACE_TEMP_COPY` when TAA or an upscaler follows, else the destination) | **Built** (5.5.4, ISHDR's `BLEND` build): the draw the engine makes when Effects11 does not replace the HDR effect, with Post Processing's passthrough or the vanilla tonemap. The fade form implies the plain one's shapes and back. |
| VL / lens flare composite (114-116) | 99024 (+0x190 helper, 100493) | VL target, lens flare target | the pre-tonemap source | **Built** (ISCompositeLensFlareVolumetricLighting) |
| SIL composite (146 / 147) | 99024 (+0x1d8, 100528) | | | Next: ISSILComposite, ISSILRawInd sources exist; same mechanism |
| IB lens flares (105, 119-122, 124) | 99024 (+0x180, 100481) | | `kIBLENSFLARES_*` | Next: raster ones through this mechanism; 121/122 are compute (below) |
| ISCopy (34, 35, 37, 38) | 99024, 99023, and the callers in R4's table | an engine target | an engine target | **Built** (5.5.4, ISCopy.hlsl dual-target): inside a scope as part of it, else each call a scope of its own. A copy from `kFRAMEBUFFER` (no SRV) is refused. ISCopyScaleBias (36) and ISCopyTextureMask (41) move the coordinates in their vertex shader, which the graph's triangle cannot: not taken. |
| VL apply (103) and the VL CS blur (106/107 via +0x188) | RenderWorldPostProcess (100430) | depth, shadow maps | `kVOLUMETRIC_LIGHTING_*` | Next. 106/107 and CS's own `ISVolumetricLightingGenerateCS` / `RaymarchCS` / `BlurHCS` / `BlurVCS` are compute: `DispatchCSShader` (75532 / 77329) with CS's replacement chosen in `CSShadersSupport::Renderer_DispatchCSShader` (Hooks.cpp). The compute form of this mechanism latches the dispatch there: CS b0 from the capture (`Find(Current(), Compute, 0)` after `OnDispatch`), the CS SRVs/UAVs from the shadow state's CS slots, group counts into a gate entry (`OpenDispatch`). |
| SSAO (SAO 132-145, `RenderSSAO` 100520 / 107245; compute set 134/135/137/145 via 100526) | RenderWorld | depth, normals, motion vectors, last frame's AO | SAO targets, then composite into kMAIN | **Built** (wave 3): CameraZ, Minify, MinifyContrast, RawAO, RawAONoTemporal, BlurH, BlurV through the explicit-target Render. The composites 138-140 since 5.5.4 (below, "5.5.4: the remainder"): the feature inputs CS's headers read are bound by published id. The compute set is not used by a vanilla frame (the census saw none) and is not taken. |
| Reflections (130 ray march, 120 mask downsample, 108/109 the compute blur) | the reflection pass 100428 / 107146 | normals mask, colour, depth, alpha, the reflection cubemap (CS, t4) | `kSSR_RAW`, the blurred pair | **Built** (wave 3): the downsample and the ray march through the explicit-target Render, the blur H/V through the compute form. The compute downsample (122) is taken by the same form when the game picks it. |
| DOF (3: 58, 11 or the CS blur 112/113, 45-47) and the distant blur (49-51) | 99023 slot 3 | downsample, swap, depth, underwater mask | the chain's target | **Built** (w3-slots): ISDepthOfField*/ISDistantBlur* and the compute blur 112/113 (ISBlurCS's DOF build), each call a scope of its own. The downsample (58) and the raster blur (11) are Next. |
| Radial blur (5: 52-54), full-screen blur (6), get hit (7: 34, 10, 40) | 99023 slots 5-7 | | | **Built**: the radial blur (w3-slots), and since 5.5.4 the blur composites' draws (ISBlur 65-71, ISNonHDRBlur 72-78), ISCopy and ISDoubleVision (dual-target), each draw outside a scope a scope of its own |
| Refraction (1: 39) | 99023 slot 1 | src, `kREFRACTION_NORMALS` | the chain's target | **Built** (w3-slots, its own scope) |
| FXAA (2) | 99023 slot 2 | | | Next (transcribed source; with the game's FXAA setting only) |
| World map (0: 43/44), Map (8: 42) | 99023 slot 0, the map menu | | | Next; the map camera is p5-menus'. ISMap's vanilla VS has no SV_Position (imagespace-shaders.md): decide whether the pass draws |
| Local map (98) | 16094 | `kLOCAL_MAP_SWAP` | `kLOCAL_MAP` | Next: one draw, src to dst, outside the end-of-frame chain: a scope detour on 16094 and the same mechanism |
| Water displacement (31: 86-94) | 31385 / 32176, the water system's displacement update | its own simulation targets | `kWATER_DISPLACEMENT*` | **Built** (5.5.4): 31385 / 32176 is a scope (`ScopeKind::Water`, two runs), ISWaterDisplacement.hlsl dual-target (the heightmaps' four neighbour coordinates made in the pixel shader). The ripple makers (88, 89) draw their own quads and are refused (`a geometry other than the full-screen quad`; Render now checks its geometry is the manager's quad). |
| Water blend (155), underwater mask (156) | +0x200, +0x208 | | | Next (155 is active CS HLSL) |
| Snow SSS (150), CopySubRegionCS (117) | RenderWorld, Main::RenderWaterEffects | | the snow swap; the water's flow map | 117 **built**: the snow copy (w3-slots: the compute form, its own scope) and the water's flow copies (5.5.4: the source by heap index in the latch); 150 Next |
| TAA (151-153) | +0x1f0 | | | Upscaling's (its TAA replacement); stays |
| Dynamic-resolution upsample (154) | +0x1f8 | | | Next, with dynamic resolution (refused today) |
| Noise (33), reflections (102, 108-109, 130-131), parallax mask blur (110-111), lighting composite (125-127), Perlin noise (128-129), debug (118, 149), simple colour (148), TAA UI/water | various | | | Next; 148 (menu 3D's clear) is p5-menus' boundary |

**Boundaries with other features.**

- **Effects11** replaces the tonemap and, when `HandlePostProcessing` returns true, the whole HDR effect is skipped: none of its
  draws reach this port. Its sun and volumetric rays (P3 W4) run at PostComposite.
- **Post Processing** runs its chain at the Tonemap anchor (its ORG port) and lets the vanilla HDR effect run with ISHDR's
  passthrough: the downsample, luminance and bloom passes still run and this port can take them; the tonemap draw stays D3D11.
- **Upscaling** replaces TAA and runs the upscaler inside 99023 after the tonemap; the ImageSpace anchor is late, so the
  separation hook runs after each batch.
- **HDR Display** owns the output transfer (its ORG port at HDROutput).
- **VolumetricLighting** replaces the VL compute shaders in the dispatch hook; their ORG form is the compute half (next).
- **p5-water**: the water displacement simulation (slot 31). **p5-menus**: menu 3D (51855's SimpleColor clear, stencil
  NOT_EQUAL composite, the manager rectangle) and the map camera; ISLocalMap is this task's. **p5-loading**: loading-screen
  frames run on the serving thread, where this port takes draws only when that thread owns rendering
  (`FrameAnchors::IsRenderOwner`, `CS_ORG_LOADING_THREAD=1`); `FrameAnchors::CanRun` checks it before any draw is withheld.

## The primary path (org-port/primary-imagespace)

With `CS_ORG_IMAGESPACE=1` the port is the only path for the effects it covers ([primary-path.md](./primary-path.md)). Until
wave 3 it learnt its passes in play: the engine drew every effect for a 120-frame window, the client registered the shapes it had
seen, waited for their programs and for its graph to be built, and a shape first seen later drew on the engine until the next cell
change. The census run `1111-131713-p35-p5-all-census-misc-fin` shows what that costs: the first registration (21 passes, in the main
menu) did not hold the SAO and reflection chains, so they drew on the engine until three later registrations, each a graph build in
play.

### Why the shapes stay passes

The design the task suggested (a pass per effect slot that reads and writes every engine target ImageSpace can touch, the target and
inputs picked per run from latched data) does not fit ORG:

- A pass declares its render targets and its reads when its graph is built (`PassBuilder::RenderTarget`, `ShaderResource`). A run can
  only draw into a view the pass declared, and one pass cannot declare the same image as a target and as a read, so every ping-pong
  (kMAIN and kMAIN_COPY, the luminance chain's alternating level, SAO's AO history) still needs a pass per direction.
- A pass's push words (its SRV and sampler handles) are part of its kept recording. Choosing them per run means re-recording on each
  change (the revision would have to read live state per frame slot), or reading every handle from a latch, a change to
  `Common/OrgResources.hlsli` that every dual-target shader shares.
- The render area is the viewport (`RasterProgram::Viewport`, "BasicRHI has no separate dynamic viewport yet"), also recorded.

So a pass stays one shape: effect, output (one or two targets, or one mip), inputs, samplers, blend state and viewport. What changes
is where the set of shapes comes from: it is known before the session's first frame, and it never grows during play.

### Where the shapes come from: the shape manifest

- **Value keys.** A shape names everything by value: the effect by its `kEffects` index (never the engine's shader object), targets
  by `RE::RENDER_TARGET` (with the mip, the depth or cube bit), samplers by their address and filter modes, the blend state by its
  four engine table fields (mode, alpha to coverage, write mode, extra), the viewport in pixels. Two sessions name the same shape the
  same way.
- **The manifest.** Every shape the port sees is written to `Data/ShaderCache/v2/ImageSpaceShapes.json` (beside the shader store), with
  its place in the frame (scope and draw index), and each seen effect's program defines. The file is rewritten whenever the session
  has seen shapes since the last write (new ones, or known ones whose last session it updates), at most every 300 frames, so the
  world's shapes are written after the main menu's. A shape not seen for 16 sessions is dropped; at most 256 are kept.
- **Setup registers the manifest.** `ImageSpace::Setup` (startup, before the main menu) reads the manifest, makes a pass for each
  shape whose targets exist (with the shapes the RE says each implies: the kMAIN / kMAIN_COPY swap, the luminance chain's alternating
  level, every bloom radius), and registers the client once. The anchor's graph is built with it at the first build point, in the
  main menu. There is no learning window, no joining phase and no cell-change join: from the first world frame every registered shape
  draws on the graph.
- **A shape outside the manifest** (the first session after a new install, a new resolution or Upscaling mode, an effect the tour
  never reached) is refused for the session: the engine draws it, the reason is logged once per effect (`a shape this session did not
  register; the manifest has it from the next session`), and the shape goes into the manifest. No graph build follows during play
  (FrameAnchors has no asynchronous anchor build to switch in at a snapshot, so the rebuild rule leaves no other choice). Under the
  flush latch such a draw is lost: the first run of a new variant fills the manifest, and runs after it are the ones to judge.

### Programs before the first world frame

Programs are keyed by value, `(effect, output format, second output format, blend fields)` for a raster effect and the effect for a
compute one. Setup queues every registered shape's program on the program build thread (`ProgramCache::Get`), which compiles its
shaders into the shader store. The gate is the client's first graph build: each pass's `make` takes its program with
`ProgramCache::GetNow`, which returns a built program, waits for one still building, or builds a missing one on the render thread
(logged once per program: `built <program> on the render thread`). That build is at the first build point after Setup, in the main
menu, so a world frame never waits for a program and never leaves a draw to the engine because one is building. A program that fails
to build leaves its pass out of the graph (logged as an error); its shape's draws are refused for the session.

**What is read at the build, not at Setup.** The first run (`1111-145713-p35-p5-all-census-misc-prim`) showed that the engine fills
its blend and sampler tables after CS's Setup: every shape whose blend fields or sampler modes were turned into ORG state there was
dropped (`no engine blend state`, `a sampler has no ORG form`), and settled Whiterun sent 33 ImageSpace draws a frame to D3D11. Setup now
imports the targets only. The blend state, the samplers and the program are taken in the pass's `make`, at the graph's build. Some
engine effect objects are made later still (ISCopySubRegionCS's): their program defines come from the manifest, which saves each
seen effect's defines. A pass left out of a build (an unfilled table, no defines, no program) has its shape's draws refused
(`its pass was left out of the graph's build`), never withheld. When a pass was left out because a table was still empty, the
client's graph is built once more (`FrameAnchors::Refresh`) at the next frame outside the world in which the tables hold what the
passes need; in a world frame it is left for the next session (logged), never rebuilt in play.

### Resize

The engine makes its targets again on a resolution change. Each frame the port compares the textures behind the registered imports
with the engine's (one read per target used, no D3D11 call); when one changed, `FrameAnchors::EngineTarget` imports the new texture,
which already asks every anchor's graph to be built again, and the client registers the same shapes on the new imports into that same
build. The draws between the change and that build (one build point) are refused by `FrameAnchors::CanRun`. The port adds no build of
its own: the import's build is the framework's, as for every reader of engine targets.

### Reads of the D3D11 context

Under the flush latch the guest context's state is stale. The draw's shape is read from the engine's shadow state
(`RendererShadowState`: render targets, depth state, PS and CS slots, sampler modes, blend fields) and the captured constants; the
viewport comes from the shadow state's `viewPort` (+0x70), not `RSGetViewports`. What is still queried from D3D11 objects (a view's
description and resource, to find the target a mip view belongs to) reads immutable objects, not context state.

## How the built part works

**Scopes.** Draws are taken only inside scopes, detoured at their function starts: the HDR effect's Render and
RenderPreTonemapComposites, RenderSSAO and the reflection pass (below), and the own-scope effects (below). Inside a scope, `BSImagespaceShader::Render` records which shader is drawing; an effect is eligible
when it is selected and the capture is active, and it has no manager rectangle and no dynamic-resolution quad. The two reads
are CommonLib's fields, checked in Ghidra on SE and AE: the manager's `rect` (Render applies it when `left` and `right` are not
-1, the engine's own test of two of its four fields) and `RUNTIME_DATA::unk58` (`RenderEffect` draws the quad at +0x50 instead
of +0x48). A custom viewport (`ImageSpaceEffect::unk88`: Render skips `UpdateViewPort`, so the draw keeps the viewport the state
holds) is no longer a refusal: the draw's viewport is read at the choke point, whoever set it.

**The choke point.** In `DrawTriShape`, an eligible effect's draw reads the shadow state as SetDirtyStates is about to apply it:
render target 0 (an engine target, no other target, no depth or stencil, no clear at the bind), no scissor, the PS textures the
effect reads (each an engine target's SRV), their address and filter modes, and the blend state from the engine's table. Then
it calls `SetDirtyStates(false)` itself and reads the viewport from the shadow state (`viewPort`, what that flush applied): that is the viewport the
engine's draw uses, whether Render set it (the whole target), the caller did (the explicit-target Render), the effect kept it
(`unk88`) or it is the dynamic-resolution part of the target (Upscaling sets the engine's dynamic-resolution ratios, so SAO and
the reflections draw into the top-left part of their targets). The viewport must be whole pixels (within 1/64, as the
dynamic-resolution sizes are float products), inside the output, with depth 0 to 1. All of that is the draw's shape. A shape
with a pass takes the draw's constants (`Constants::Capture(kDrawStages)`, then `Find(use, Pixel, 2)` and `Find(use, Pixel,
12)`) and joins the batch instead of drawing. Anything else is drawn by the engine, after the pending batch runs.

**Mip targets.** The SAO camera-Z levels (`kSAO_CAMERAZ_MIP_LEVEL_*`) have no texture of their own: each is an RTV and an SRV of
one mip of another target's texture. A shape names such a target as the texture's target with the mip (`WithMip`; logged as
`rt<n>.mip<m>`), found by the view's resource (`TargetOfTexture`) and its mip slice. The pass draws into that mip of the
texture's import (`Draw::mip`) and reads one mip (`Bind::Srv(..., mip)`), so ISSAOCameraZ and ISMinify read one level and write
the next of one graph resource. The view must have the import's format and its texture's target an RTV (output) or SRV (input);
otherwise the draw stays with the engine. A draw that reads the mip it writes, or the whole texture, reads its own output.

**Batches.** A batch runs (`FrameAnchors::Run(ImageSpace)`) before any engine draw or dispatch that is not taken, before a taken
draw whose pass is not after the last one in the batch (the graph runs passes in order-key order), and at the end of a scope.
Its commit writes each draw's constant location (`OrgEngineConstantsAt`: ring chunk SRV and offset) into the client's latch
at the draw's entry and opens its gate entry. The shaders read PerGeometry from the ring through that location
(`Common/OrgEngineConstants.hlsli`), the shared constants for FrameBuffer and SharedData, and their textures and samplers by
handle. A batch is started (its first draw withheld) only when `FrameAnchors::CanRun` says the anchor can run it now (the
caller owns rendering, the registration is in the built graph, the ring has a slot left this frame) and the frame has used
fewer runs than `FrameAnchors::RunsPerFrame(ImageSpace)` (4, from the anchor table; the steady state uses two: the
composites and the HDR effect up to its tonemap). Otherwise the engine draws. The ring holds `kGameFramesInFlight` times
the runs a frame, and `CanRun` sees a full one before a draw is withheld, so a withheld batch never meets it. Shapes get their entries (and so their order) by their place among the frame's draws, so the luminance chain's
alternating levels do not cut the batch.

**More scopes (wave 3).** RenderSSAO (100520 / 107245) and the reflection pass (100428 / 107146) are scopes too. Their draws go
through BSImagespaceShader's explicit-target Render (100951 / 107733; Ghidra, SE `0x14130cf80`, AE `0x1414f8b00`): its this is the
shader itself, its caller binds the target and the textures, and it draws its geometry argument or the manager's quad (+0x48) when
that is null. It is detoured like Render; a draw with another geometry is refused. Inputs may be an engine depth target's depth SRV
(`depthStencils[i].depthSRV`, imported with `FrameAnchors::EngineDepth`) besides render targets, and the ray march's t4 is the
reflection cubemap CS binds itself (VanillaBinding.cpp), imported with `EngineCubemap(kREFLECTIONS)`.

**The compute form.** Inside a scope, `BSImagespaceShader::DispatchComputeShader` (100952 / 107734: the shader's setup, then
`BSComputeShader::Dispatch` with technique 0, then `DispatchCSShader`) names the effect, and its `DispatchCSShader` (75532 / 77329)
is taken: the shadow state's compute slots give the shape (`CSUAV[0]` the output's render target, `CSTexture` the inputs,
`CSTextureAddressMode`/`FilterMode` the samplers), `SetDirtyStates(true)` runs as the dispatch would, the engine's CS b0
(`Constants::EngineComputeBuffer`, the Compute span of the constant-buffer block) is bound on DXVK's context so the capture records
it (`Capture(StageBit(Compute))`, `Find(use, Compute, 0)`), and the group counts open the pass's gate entry (`OpenDispatch`). The
pass is an `AnchorPasses::Dispatch` through the gate, with the CS source built by `ProgramCache<ComputeProgram>` from
`GetCompileDefines(shader, Compute, 0)`. The separable blur helper (107209/107210) sizes the groups from the target and GRIDSIZE.

**Runs a frame.** Each kind of scope has its own share of the anchor's runs a frame (`kScopeRuns` in `ImageSpace.cpp`): the
composites 1, the HDR effect 2, RenderSSAO 2, the reflection pass 2, the own-scope effects 8 (one per call) and the water's flow
copies 4 (one per copy). The anchor is sized for the sum, 19 (`FrameAnchors.cpp`); Setup warns when it is sized for fewer. A share of its own per kind means the scopes early in
the frame never use up the runs of the ones after them; the second run of SSAO, the reflections and HDR covers a batch cut short by
a draw the graph does not take. A draw whose scope used its share is refused (`its scope used its runs this frame`). Each scope's
batch still runs at the scope's end: CS code around these calls uses D3D11 and runs other anchors (the Tonemap anchor in
`Main_HDRTonemapBlendCinematic_Render`, Upscaling's and HDR Display's hooks around the end-of-frame chain). The frame slots this
costs: 19 runs times 4 frames in flight, 76 slots, within CS's budget of 512 (render-graph.md, "The slot budget"). Before
`org-port/frame-slots` ORG's slot index was 8-bit, the budget 255, and this anchor had 4 runs.

**Own-scope effects (w3-slots).** The end-of-frame chain's effects (99023: depth of field and the distant blur, refraction, the
radial blur), the depth-of-field compute blur (107734 <- 107209 <- 107208) and the snow pass's ISCopySubRegionCS (107734 <- 107217
<- RenderWorld) are drawn outside every other scope. Each is flagged `ownScope` in `kEffects`: a call of its `BSImagespaceShader::
Render` or `DispatchComputeShader` outside a scope is a scope of its own (`ScopeKind::Effect`), entered and left around the call, so
its batch runs at the call's end, before whatever the engine or CS does next. No new hook: the two effect hooks open the scope.
Called inside another scope, such an effect is part of that scope as any other. The sources are dual-target (ISDepthOfField,
ISRefraction, ISRadialBlur, ISCopySubRegionCS; ISBlurCS's DOF build already was). The copy tests its store against the output's
size on ORG. Read masks: the depth of field reads t0-t3 (t4, the mask, instead of t3 with MASKED); an input that is not an engine
target (an unbound average-depth slot, for example) keeps that effect with the engine.

**Passes.** One `AnchorPasses::Draw` per shape: the effect's RasterProgram (its CS source, the D3D11 build's defines from
`ShaderCache::GetCompileDefines`, the target's format, the engine blend state as an rhi blend), the output and inputs as
`FrameAnchors::EngineTarget` imports (the P4 targets' published textures under `CS_ORG_ENGINE_TARGETS`), samplers made from
the engine's sampler table, load op Load, the shape's viewport as the render area, and the gate entry. The ping-pong of the
chain is in the shapes: each (output, inputs, viewport) the engine uses is its own pass. The graph's fullscreen triangle gives
TEXCOORD0 from 0 to 1 over the render area, as the engine's quad does over its viewport.

**Why the capture, not ConstantMirror.** The ImageSpace constants go through the 4-buffer custom mapper, rewritten for every
draw. The capture already copies each use's window into the frame's ring at the draw, and gives a location a kept recording can
take from its latch. ConstantMirror would need a watch per custom buffer and a copy of its bytes per draw into a latch of our
own, which is what the capture does already.

## Graph rebuilds (the user rule)

| Trigger | Handling |
| --- | --- |
| Effects that run on some frames only (bloom radius changes, adaptation, composites without VL) | Each shape is a pass behind its gate entry; a run opens only the entries it draws. No rebuild. |
| Targets changing between effects (the luminance chain's alternation, ping-pong, SAO's AO history) | Separate shapes in the manifest, with the shapes the RE says they imply (kMAIN / kMAIN_COPY, the alternating last luminance level, every bloom radius), all registered at Setup. No rebuild. |
| A shape the session did not register (a new viewport, a new effect, the first session) | Refused for the session and written to the manifest; the next session registers it at Setup. No rebuild during play. |
| Programs | Queued at Setup on the program build thread; each pass takes its program at the graph's first build (`ProgramCache::GetNow`), in the main menu. Never a rebuild. |
| The engine's targets made again (a resize) | The new import (`FrameAnchors::EngineTarget`) already asks every anchor's graph to be built again; the client registers the same shapes on the new imports into that build. No build of its own. |
| Registration itself | Once, at Setup, before the main menu's first build point. |

## Fallbacks and failure

- Per draw, to the engine (each reason logged once per effect, `Util::ReasonLog`): not selected, ineligible (above), a shape the
  shadow state does not describe fully, a shape the session did not register, a scope that used its runs, an anchor whose graph
  does not hold the registration yet (before the first build, or for one build point after a resize) or whose ring is full. These
  are inherent refusals, at info.
- Failures, at error (`<effect> failed on the graph and stays with the engine`): constants the capture does not hold for the draw,
  or a ring chunk missing. On the primary path these should not happen.
- Per shape, at Setup: an import that fails, a viewport outside this resolution's target, a sampler or blend without an ORG form
  (blend factor, dual source): `<effect> into <target> has no pass this session (<reason>)`. A program that does not build leaves
  its pass out of the graph (error).
- Under the flush latch (flush.md) no ImageSpace draw is vouched for: a draw the engine still makes (an effect the port does not
  cover, any refusal or failure above) is skipped with its `[E]` line and counted by the census. Draws of effects the port does not
  cover are named every 300 frames (the summary line, "5.5.4: the remainder"). Volumetric Lighting's render, apply and blurs are its own port's (5.5.3). 5.5.3 deleted `imagespace: volumetric
  lighting compute`; 5.5.4 deleted `shape refused`, `shape not in the manifest`, `failed on the graph` and `effect not on the
  graph`.
- A failed anchor epoch (`onFailed`) returns every effect to the engine for the session. A batch that does not draw after `CanRun`
  allowed it loses those draws for that frame (counted as `missed`, logged as an error).

## Switches and log lines

| Switch | Effect |
| --- | --- |
| `CS_ORG_IMAGESPACE=1` | Installs the hooks, registers the manifest's shapes at Setup and takes their draws. Needs `CS_ORG_ENGINE_CONSTANTS=1`. |
| `CS_ORG_IMAGESPACE_EFFECTS=<list>` | A diagnostic for bisecting an effect. Comma list of engine names, effect names or groups (`hdr`, `bloom`, `blur`, `nonhdrblur`, `tonemap`, `iscopy`, `gethit`, `composite`, `sao`, `ssr`, `dof`, `refraction`, `radialblur`, `copy`, `waterblend`), any case. Unset or `all`: every covered effect. |
| `CS_ORG_IMAGESPACE_LOG=1` | A diagnostic: logs each shape new to the manifest (with its viewport), and each shape's constant sizes once when its first draw is taken. |

Covered effect names: the HDR group (ISHDRDownSample16, ISHDRDownSample4 (also into kLDR_DOWNSAMPLE0 outside the HDR effect, a scope of
its own), the Lum, LumClamp, RGB2Lum and LightAdapt forms), ISBrightPassBlur3 to 15, ISBlur3 to 15 and ISNonHDRBlur3 to 15, the tonemap
(ISHDRTonemapBlendCinematic and its Fade form), ISCopy, ISCopyDynamicFetchDisabled, ISCopyCustomViewport, ISCopyGrayScale, ISDoubleVision,
the three composites, the SAO chain with its
composite (ISSAOCompositeSAO, ISSAOCompositeFog, ISSAOCompositeSAOFog), the reflection pass, the depth of field and distant blur
(into kFRAMEBUFFER too) with their compute blur, ISRefraction, the radial blurs, ISCopySubRegionCS (the snow pass's and the water's
flow copies) and ISWaterBlend (two targets: kMAIN and the water history).

Log lines (all `[ORG] ImageSpace:`): `hooks installed (...)`; `on (CS_ORG_IMAGESPACE); effects <list>`; `the shape manifest holds <n>
shapes (session <k>; ...)` or `no shape manifest yet`; `registered <n> passes (registration <k>); the anchor's graph holds them from
its next build`; `built the program of <pass> on the render thread at the graph's build` (a program Setup could not queue in time);
`the first batch (<n> draws) ran on the graph`; `made the water's flow map (<n>x<n>) for the flow copies' pass`; `<pass>: <id> is not
on the graph (its feature's ORG switch is off); its pass is left out` (the SAO composite without IBL's, the fog's or Dynamic
Cubemaps' ORG path); `<effect> stays with the engine: <reason>`; `wrote the shape manifest (<n> shapes,
<m> seen this session)`; `the engine made its targets again; the <n> passes register again on the new imports`; errors: `<effect>
failed on the graph ...`, `<pass> has no program`, `... every ImageSpace effect returns to the engine`.

## Test plan

Scenarios `tools/cs-ab/scenarios/imagespace-org0.env` (D3D11, the capture on) and `imagespace-org1.env` (every covered effect):
Whiterun by day and at night, the world map, the journal (menu blur), Dragonsreach, Riverwood. Bisection:
a copy of `imagespace-org1.env` with `CS_ORG_IMAGESPACE_EFFECTS` set to a group (`hdr`, `bloom`, `composite`) or any effect name.

1. The log shows the startup lines, `registered`, `the graph holds` and `the first batch`; no ImageSpace error, no `Render graph
   disabled`, no `[DCLF] Forced off`.
2. Shots match imagespace-org0's: exposure (luminance and adaptation) and bloom by day and at night, VL in Whiterun's day.
3. Refusals are expected for the tonemap (not covered), and for effects inside menus with a rectangle.
4. With `CS_ORG_IMAGESPACE_LOG=1`, the shapes stop growing after the first window; Dragonsreach (a cell change) may add a few.

## The P5.13 residue (wave 3)

The census run `1111-072658-p35-p5-all-census-w3` (`p5-all-census.env`, `CS_ORG_IMAGESPACE=1`) showed per frame ImageSpace raster
draws 25-30 and ImageSpace compute dispatches 4-7 reaching D3D11. By class (the three game frames above the draw, the last
technique, the target), with what wave 3 does about each:

| Class | Per frame | Now |
| --- | --- | --- |
| 107733 <- RenderSSAO: ISSAOCameraZ, ISMinify x10, ISMinifyContrast, ISSAORawAO(NoTemporal), ISSAOBlurH/V | 14-15 | Graph (one batch; a second if one is cut) |
| 107733 <- RenderSSAO: ISSAOCompositeFog / SAO / SAOFog into kMAIN | 1 | Graph since 5.5.4 (the feature inputs by published id) |
| 107733 <- the reflection pass: ISDownsampleIgnoreBrightest, ISReflectionsRayTracing | 2 | Graph (one batch with the blur) |
| 107734 <- 107209/107210 <- 107207: the reflection blur H/V | 2 | Graph (compute form) |
| 107732 <- HDR Render: downsample, luminance, adaptation, bloom | 7 where refused | Graph as before; two runs kept for it |
| 107732 <- HDR Render: the tonemap into kFRAMEBUFFER | 1 | D3D11: CS's tonemap owners |
| 107733 ISWaterBlend into kMAIN (two targets; CS clears the second with D3D11 first) | 1 | D3D11 (below) |
| 107732 <- 99023: ISDistantBlur, ISRefraction, ISRadialBlur | 1-2 | Graph (w3-slots: own scopes). ISDistantBlur and ISRefraction keep their own viewport (`unk88`); since fix-imagespace-tail they are taken with the viewport the context holds. A draw into kFRAMEBUFFER (the chain's last effect) stays D3D11: no engine target. |
| 107734 <- 107209 <- 107208: the DOF / distant blur compute | 1 | Graph (w3-slots: own scope, the compute form) |
| 107734 <- 107217 <- RenderWorld: ISCopySubRegionCS (snow) | 1 | Graph (w3-slots: own scope, the compute form) |
| 107734 <- RenderWaterEffects: ISCopySubRegionCS (BGSAutoWater's queue) | 0-1 | Graph since 5.5.4 (the flow map imported, the source by heap index) |
| 107733 <- RenderSSAO and the reflection pass, with Upscaling | (the SAO and SSR rows) | Graph since fix-imagespace-tail: the dynamic-resolution viewport is the pass's render area, and the camera-Z levels are mips of one import. |
| 107023 <- Main::Draw: the volumetric lighting compute (VolumetricLighting's CS) | 2-3 | Volumetric Lighting's port (volumetric-lighting.md) |

So with `CS_ORG_IMAGESPACE=1` the census classes that should drop to zero are: the `ImageSpace` draws in DrawTriShape whose frames
are `107637 <- 107644 <- 107733` and whose target is a `kSAO_CAMERAZ_MIP_LEVEL_*`, `kSAO_RAWAO*`, `kSAO_TEMP_BLUR`, `kSAO`,
`kNORMAL_TAAMASK_SSRMASK_DOWNSAMPLED` or `kSSR_RAW` target, and the dispatches `77329 <- 107734 <- 107209/107210 <- 107207`. The
HDR classes (107732 into `kHDR_*`) stay at zero once the client is live. Since w3-slots these drop too: the draws
`107637 <- 107644 <- 107732 <- 99023` (the distant blur or depth of field, refraction, the radial blur) and the dispatches
`77329 <- 107734 <- 107209 <- 107208` (the depth-of-field blur) and `77329 <- 107734 <- 107217` (ISCopySubRegionCS). An effect
whose shape is refused stays (its reason is logged once). Scenarios: `imagespace-census.env` (and
`imagespace-census-org0.env` for the same tour with the port off), `imagespace-sao-ssr-org0.env`/`-org1.env` for the shots.

## What stays the engine's

The final-build census (flush.md, "The census after wave 3") left seven ImageSpace draws and dispatches and two volumetric lighting
dispatches a frame in settled Whiterun. Most of the ImageSpace ones were shapes first seen after the learning window's registration
(`1111-131713-p35-p5-all-census-misc-fin`: the whole SAO and reflection chains until registrations 2 to 4); with the manifest they
are passes from Setup. By class:

| Class | Now |
| --- | --- |
| DistantBlur (99023's last effect) into `kFRAMEBUFFER` | Taken: `kFRAMEBUFFER` is an output (`EngineTarget` imports the RTV's texture); the refusal was `its output is not an engine render target` (index 0 was outside the range checked). |
| HDRDownSample4 into `kLDR_DOWNSAMPLE0` (Render 107732 outside the HDR effect) | Taken when its input is an engine target: ISHDRDownSample4 is an own-scope effect outside the HDR effect. If its input is not one, the refusal says so (`an input that is not an engine target`). |
| ISWaterBlend into kMAIN and the water history | Taken: two-target passes (`AnchorPasses::Draw::extraTargets`), ISWaterBlend.hlsl dual-target, own scope through the explicit-target Render. |
| Refraction's late shape (ISRefraction into kMAIN_COPY at the full output size) | Taken from Setup (5.5.4): every ISRefraction (and ISDistantBlur) shape implies its whole-target shape, sized from the target table. |
| ISSAOCompositeFog / SAO / SAOFog into kMAIN | Taken (5.5.4): ISSAOComposite.hlsl dual-target, the feature inputs by published id. |
| ISApplyVolumetricLighting (two raster draws into `kIMAGESPACE_VOLUMETRIC_LIGHTING` / `_PREVIOUS`) | **Volumetric Lighting's port** (`CS_ORG_VOLUMETRIC_LIGHTING`, [volumetric-lighting.md](./volumetric-lighting.md)): ApplyVL (SE 100475, AE 107193) is detoured and drawn at the VolumetricLighting anchor, reading the port's graph volume; the repartition texture is computed in the shader. Not this port's: its inputs are the VL port's. |
| The VL blur (ISVolumetricLightingBlurHCS / VCS through 107206 -> 107209/107210 -> 107734) | **Volumetric Lighting's port**: 107206 (SE 100486) is detoured and its two dispatches are graph passes with the VL data in the port's latch. |
| The VL dispatches from AE 107023 (SE 100306) | **Volumetric Lighting's port**: the VL render is detoured, its dispatches withheld at Hooks.cpp's DispatchCSShader seam, and the generate and raymarch run at the VolumetricLighting anchor with the engine's PerTechnique latched. This port's 107023 detour and the reason `imagespace: volumetric lighting compute` are deleted. |
| ISCopySubRegionCS from `Main::RenderWaterEffects` (BGSAutoWater's queue) | Taken (5.5.4): the flow map is an import made at Setup, and each copy's loaded texture is read by its heap index from the latch. |
| The tonemap (55, 56) | Stays: CS's tonemap owners (Effects11, Post Processing, HDR Display). |
| A fractional viewport, the dynamic-resolution quad (`unk58`), a manager rectangle (menu 3D) | Stay, as before (inherent refusals). |

## 5.5.4: the remainder

Plan item 5.5.4 (org-full-port.md, Phase 5.5) without the volumetric lighting (5.5.3's lane owns VolumetricLighting,
ISApplyVolumetricLighting and the VL blurs).

**The SAO composite.** RenderSSAO (SE `0x1412ebd70`) binds seven engine targets before the composite's explicit-target Render: t0 the
source, t1 kSAO, t2 the main depth's SRV, t3 the normals' SSR mask (TAAMASK_SSRMASK_SWAP), t4 `renderTargets[0x70]` (the snow's
specular alpha), t5 `renderTargets[18]` (the shadow mask), t6 the reflections target; samplers 0-5 address 0 filter 0, sampler 6
filter 1. So the shape is read as for any other effect (`kMaxSlots` is 7 now; manifests with five-entry arrays still read). Its source
includes `IBL/IBL.hlsli` and `ExponentialHeightFog/ExponentialHeightFog.hlsli` (and through it `DynamicCubemaps.hlsli`). Those headers
were already dual-target (`ORG_HEADER_SRV`; Water.hlsl and Sky.hlsl include them on ORG), so only ISSAOComposite.hlsl changed: an ORG
resource list with the seven SRVs, the seven samplers, s9 (`SampColorSampler`), five `EXTERN` entries (EnvIBLTexture, SkyIBLTexture,
the fog's two volumes, Dynamic Cubemaps' EnvTexture) and two `UNREAD` ones, and PerGeometry through the latch. The D3D11 build is
identical (`verify-shader-refactor.ps1`). `EffectInfo::features` marks the effect; `MakePass` adds, after its samplers, D3D11's
default sampler for s9 and, for each header the program's defines include (`IBL`; `EXP_HEIGHT_FOG` with `APPLY_FOG`; with
`DYNAMIC_CUBEMAPS`), `AnchorPasses::Bind::Published(id)` for `IBL::EnvIBL`, `IBL::SkyIBL`,
`ExponentialHeightFog::IntegratedLightScattering` / `FarIntegratedLightScattering` and `DynamicCubemaps::EnvTextureBC6H`. The pass
declares those reads, so ORG orders it after the features' Prepass writes. A feature on D3D11 (its ORG switch off) leaves the pass
out, logged, and the composite's draws are refused. The shader's own settings checks (IBL enabled, volumetric fog on, interiors with
Dynamic Cubemaps) decide what is read, as on D3D11, where the slots hold null in the scenes those checks skip.

**The water's flow copies.** `Main::RenderWaterEffects` (SE `0x1405b2300`) pops BGSAutoWater's queue and, per queued flow texture,
calls `DispatchCopySubRegion` in mode 0: CS t0 the texture's SRV (`BSGraphics::Texture+0x10`), u0 the flow map's UAV (+0x08). The
flow map is the NiSourceTexture "FlowMap" at SE 527694 / AE 414616, made by SE 31234 (inlined in AE `0x1405195d0`) only when the slot
is empty, at the first cell with flowing water. To have it in the session's registration (no graph build in play), Setup makes it
the same way: `Renderer::CreateTexture2D` (75511 / 77303) of zeros, the side the setting value at SE 501245 / AE 359676 times 64,
R8G8B8A8_UNORM with a UAV (75533 / 77330), held by the NiSourceTexture (69335 / 70717). The engine then uses it. The shape is
`ISCopySubRegionCS` into `kFlowMapOutput` (`FrameAnchors::D3D11Texture("engine.flow-map", ...)`) from `kLatchSourceInput`, registered
at Setup without the manifest. At the dispatch the source view is resolved to a heap index (`DCLF::GpuTextures::ResolveBinding`,
which takes an ORG engine texture's image or imports DXVK's) and written into the latch (`OrgEngineConstantsAt::Source`, which the
shader reads instead of t0); the binding's owner is held per frame slot until that slot's latch is written again. Each copy is its
own scope (`ScopeKind::FlowMap`, four runs a frame); a fifth copy in one frame is refused. Unified Water NOPs these copies when its
mesh is valid (`DisableVanillaWaterLOD`), so most sessions have none.

**Refraction's late shape.** ISRefraction and ISDistantBlur keep the viewport the effect before them left (`unk88`), so they draw a
part of their target or all of it. `AddImpliedShapes` adds, for each such shape, the whole-target one (`EffectInfo::keepsViewport`,
the size from the target table at Setup), before the kMAIN / kMAIN_COPY swap: ISRefraction into kMAIN_COPY at the full size is a
pass from the first session that saw any ISRefraction shape.

**The distant blur** into kFRAMEBUFFER is a shape like any other (`ResolveOutput`'s kFRAMEBUFFER case, `EngineTarget(0)` importing
the RTV's texture); no reason covers it, and its whole-target shape is implied too.

**The rest of the chain.** The census after the first 5.5.4 merge (`1112-021322-p35-latch-census-short-p5-now`) still showed 3-6
ImageSpace draws a frame under `effect not on the graph` in every window, into `kMAIN`, `kHDR_BLOOM`, `kIMAGESPACE_TEMP_COPY` and
`kFRAMEBUFFER`. By the RE: the bloom's second draw is the plain blur (ISBlur 65-71) into `kHDR_BLOOM`; the tonemap writes
`kIMAGESPACE_TEMP_COPY` when TAA or an upscaler follows, else the destination; the copies go back into `kMAIN` and to the frame
buffer. So the plain and non-HDR blurs, the tonemap and its fade form (ISHDR's `BLEND` build was already dual-target), ISCopy's
pass-through forms and the get-hit double vision are covered now: ISCopy.hlsl and ISDoubleVision.hlsl got ORG builds (the D3D11
builds are unchanged, all ORG code is under `CS_ORG_BINDLESS`). The blurs and the tonemap's two forms imply each other's shapes
(`SharesShapes`); the copies and the tonemap make no swap pair (one source into several targets by caller, `PingPongs`). The copies,
the blurs and the double vision called outside every scope are scopes of their own (24 runs a frame for those, two for the
pre-tonemap composites).

**Reasons.** `shape refused`, `shape not in the manifest`, `failed on the graph` and `effect not on the graph` are deleted. Any
ImageSpace draw that still reaches the engine is skipped by the latch and counted by the census, and an effect the port does not
cover is named in the 300-frame line (Fallbacks and failure).

**The summary line.** `[ORG] ImageSpace: draws and dispatches left to D3D11 in <n> frames: <shader> into rt<i> depth <d> (<why>):
<count>, ...`, every 300 frames. It counts every ImageSpace draw the engine makes on D3D11: inside a hooked ImageSpace call, named by
that call's shader, with `not covered` or `covered, <the refusal or failure>`; outside them, when the technique is an ImageSpace shader
(`State::currentShader`, such as a BSBatchRenderer pass), with `outside the hooked ImageSpace calls`; and `during a batch's run` or
`not the render owner` when the port did not look at the draw.

**Left with the engine, by reason:** ISCopyScaleBias and ISCopyTextureMask (their vertex shaders move the coordinates); a copy
from `kFRAMEBUFFER` (no SRV: the main frame's screenshot copies); ISSimpleColor and the menu 3D chain (the menu views lane, a
manager rectangle); ISMap, ISWorldMap, ISLocalMap (the map views, p5-menus); ISAlphaBlend (the race menu's tint masks, loaded
textures, not engine targets); the lens flares, IB lens flares, SIL composite, FXAA, noise, snow SSS, TAA forms, the
dynamic-resolution upsample, ISUnderwaterMask and the water displacement's ripple makers until the 300-frame line names them in play.

**Rebuilds.** None during play from this work. A feature id the composite declares that is published after the graph's first build
builds every graph once (`PublishedResources::IsDeclarable`), at that publish (in the main menu for the ported features).

**Not verified without a run:** the flow map made at Setup (that the setting is loaded by then, and that the engine's flow path
accepts the early texture); the ordering between the flow copy's write (the anchor's import) and the water's read of the same image
through Drawcall Limit Fix's import (two graph resources on one image); the SAO composite's s9 (D3D11 leaves whatever an earlier draw
bound there).

Scenarios: `imagespace-rest-census.env` (the latch census, a short tour with Riverwood's river) and `imagespace-rest-org1.env` /
`-org0.env` (shots, the port on and off).

### The darker Dragonsreach (`imagespace-sao-ssr-org1.env`)

The suspicion was the eye-adaptation chain split between the graph and D3D11. The run `1111-120550-p35-imagespace-sao-ssr-org1-sm2`
does not support it: after `coc WhiterunDragonsreach` the log shows no new shape but ISRefraction's, so every HDR draw in
Dragonsreach was the graph's, with the same shapes as in Whiterun (the last luminance level alternating rt30 / rt32, read by the
D3D11 tonemap). What does change in an interior is SAO: interiors use the full-strength composite (ISSAOCompositeSAO, D3D11, reading
kSAO), while Whiterun's ISSAOCompositeFog barely shows it. So the graph's SAO chain (camera Z and its mips, RawAO with its history
ping-pong rt62 / rt64 and the normals rt4 / rt5, the blurs into kSAO) is the first suspect, not the HDR chain. With the whole HDR
chain on the graph from Setup the split cannot happen any more in a registered session either. To tell: `primary-imagespace-dr-sao.env`
and `primary-imagespace-dr-hdr.env` take only the SAO or only the HDR group; the darker one names the chain.
