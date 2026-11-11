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
| Bright-pass bloom (23-29 / 79-85) | HDR Render | downsample0, swap, luminance | `kHDR_BLURSWAP`, `kHDR_BLOOM` | **Built** (ISBlur) |
| Tonemap (55, 56) | HDR Render | bloom, source, luminance | the HDR output | Stays: CS's Tonemap hook, Effects11 (replaces the whole HDR effect when on), Post Processing (its own ORG chain at the Tonemap anchor; ISHDR's passthrough). Next: the vanilla tonemap only when neither feature owns it, through this mechanism (ISHDR is already dual-target for `BLEND`). |
| VL / lens flare composite (114-116) | 99024 (+0x190 helper, 100493) | VL target, lens flare target | the pre-tonemap source | **Built** (ISCompositeLensFlareVolumetricLighting) |
| SIL composite (146 / 147) | 99024 (+0x1d8, 100528) | | | Next: ISSILComposite, ISSILRawInd sources exist; same mechanism |
| IB lens flares (105, 119-122, 124) | 99024 (+0x180, 100481) | | `kIBLENSFLARES_*` | Next: raster ones through this mechanism; 121/122 are compute (below) |
| ISCopy back to dst (34) | 99024, 99023, and the callers in R4's table | | | Next: ISCopy is a plain copy; a graph Copy where formats match, else the shader. Menu 3D's and the framebuffer copies need the frame's present path (P5.8, p5-menus) |
| VL apply (103) and the VL CS blur (106/107 via +0x188) | RenderWorldPostProcess (100430) | depth, shadow maps | `kVOLUMETRIC_LIGHTING_*` | Next. 106/107 and CS's own `ISVolumetricLightingGenerateCS` / `RaymarchCS` / `BlurHCS` / `BlurVCS` are compute: `DispatchCSShader` (75532 / 77329) with CS's replacement chosen in `CSShadersSupport::Renderer_DispatchCSShader` (Hooks.cpp). The compute form of this mechanism latches the dispatch there: CS b0 from the capture (`Find(Current(), Compute, 0)` after `OnDispatch`), the CS SRVs/UAVs from the shadow state's CS slots, group counts into a gate entry (`OpenDispatch`). |
| SSAO (SAO 132-145, `RenderSSAO` 100520; compute set 134/135/137/145 via 100526) | RenderWorld, RenderPlayerView | depth, normals | SAO targets, then composite into kMAIN | Next: the composites 138-140 are raster (active CS HLSL); the raw AO and its blurs are DXBC-transcribed sources. Interior and exterior. |
| DOF (3: 58, 11 or the CS blur 112/113, 45-47) | 99023 slot 3 | downsample, swap, depth, underwater mask | the chain's target | Next: raster children through this mechanism; it runs only with DOF on (gated, no rebuild) |
| Radial blur (5: 52-54), full-screen blur (6), get hit (7: 34, 10, 40) | 99023 slots 5-7 | | | Next: same mechanism; menu blur is FullScreenBlur (the journal) |
| Refraction (1: 39) | 99023 slot 1 | src, `kREFRACTION_NORMALS` | the chain's target | Next |
| FXAA (2) | 99023 slot 2 | | | Next (transcribed source; with the game's FXAA setting only) |
| World map (0: 43/44), Map (8: 42) | 99023 slot 0, the map menu | | | Next; the map camera is p5-menus'. ISMap's vanilla VS has no SV_Position (imagespace-shaders.md): decide whether the pass draws |
| Local map (98) | 16094 | `kLOCAL_MAP_SWAP` | `kLOCAL_MAP` | Next: one draw, src to dst, outside the end-of-frame chain: a scope detour on 16094 and the same mechanism |
| Water displacement (31: 86-94) | 31385, the water system | its own simulation targets | `kWATER_DISPLACEMENT*` | **p5-water's** (P5.4): the water family owns its simulation. The mechanism here is reusable: a scope on 31385 with the 86-94 sources dual-target. |
| Water blend (155), underwater mask (156) | +0x200, +0x208 | | | Next (155 is active CS HLSL) |
| Snow SSS (150), CopySubRegionCS (117) | RenderWorld | | | Next; 117 is compute |
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
  frames run on a job thread, where this port never takes a draw (`FrameAnchors::OnMainThread`).

## How the built part works

**Scopes.** Draws are taken only inside two scopes, detoured at their function starts: the HDR effect's Render and
RenderPreTonemapComposites. Inside a scope, `BSImagespaceShader::Render` records which shader is drawing; an effect is eligible
when it is selected and the capture is active, and it has no custom viewport, no manager rectangle and no dynamic-resolution
quad.

**The choke point.** In `DrawTriShape`, an eligible effect's draw reads the shadow state as SetDirtyStates is about to apply it:
render target 0 (an engine target, no other target, no depth or stencil, no clear at the bind), a viewport covering the target,
no scissor, the PS textures the effect reads (each an engine target's SRV), their address and filter modes, and the blend state
from the engine's table. That is the draw's shape. A shape with a pass then calls `SetDirtyStates(false)` itself, takes the
captured PS b2 (`Constants::Find(Current(), Pixel, 2)`, group Custom), and joins the batch instead of drawing. Anything else is
drawn by the engine, after the pending batch runs.

**Batches.** A batch runs (`FrameAnchors::Run(ImageSpace)`) before any engine draw or dispatch that is not taken, before a taken
draw whose pass is not after the last one in the batch (the graph runs passes in order-key order), and at the end of a scope.
Its commit writes each draw's constant location (`OrgEngineConstantsAt`: ring chunk SRV and offset) into the client's latch
at the draw's entry and opens its gate entry. The shaders read PerGeometry from the ring through that location
(`Common/OrgEngineConstants.hlsli`), the shared constants for FrameBuffer and SharedData, and their textures and samplers by
handle. Draws are taken for two batches a frame (the composites, the HDR effect before its tonemap), and a later batch's draws
stay with D3D11. The anchor is sized for four runs (the joining runs use the rest), but the runtime sizes every multi-run
anchor for two when the anchors' frame slots would pass 255, and a batch must never meet a full ring after its draws were
withheld. Shapes get their entries (and so their order) by their place among the frame's draws, so the luminance chain's
alternating levels do not cut the batch.

**Passes.** One `AnchorPasses::Draw` per shape: the effect's RasterProgram (its CS source, the D3D11 build's defines from
`ShaderCache::GetCompileDefines`, the target's format, the engine blend state as an rhi blend), the output and inputs as
`FrameAnchors::EngineTarget` imports (the P4 targets' published textures under `CS_ORG_ENGINE_TARGETS`), samplers made from
the engine's sampler table, load op Load, and the gate entry. The ping-pong of the chain is in the shapes: each (output,
inputs) pair the engine uses is its own pass.

**Why the capture, not ConstantMirror.** The ImageSpace constants go through the 4-buffer custom mapper, rewritten for every
draw. The capture already copies each use's window into the frame's ring at the draw, and gives a location a kept recording can
take from its latch. ConstantMirror would need a watch per custom buffer and a copy of its bytes per draw into a latch of our
own, which is what the capture does already.

## Graph rebuilds (the user rule)

| Trigger | Handling |
| --- | --- |
| Effects that run on some frames only (bloom radius changes, adaptation, composites without VL) | Each shape is a pass behind its gate entry; a run opens only the entries it draws. No rebuild. |
| Targets changing between effects (the luminance chain's alternation, ping-pong) | Learnt as separate shapes in the learning window (120 frames without a new shape), all registered at once. No rebuild. |
| A shape first seen after the registration (a new weather's bloom radius, an effect's first use) | Drawn by D3D11 and queued; it joins only at the next cell change (a loading screen has just rebuilt things anyway), at most 16 registrations a session. |
| The engine's targets made again (a resize) | The passes are dropped and the shapes learnt again; the anchors rebuild for the new imports anyway. |
| Registration itself | Once per learning window. The draws stay the engine's until `FrameAnchors::InBuiltGraph` says the graph holds it; the port only runs the empty anchor (the anchors' joining rule) so the host builds it. How long that build holds the frame is the anchors' behaviour, the same as for every client; it happens once per registration, not per frame. |

## Fallbacks and failure

- Per draw, to D3D11 (each reason logged once per effect): not selected, ineligible (above), a shape the shadow state does not
  describe fully, a shape without a pass, the frame's runs used up, no captured constants.
- Per shape, at registration: an import that fails, a sampler or blend without an ORG form (blend factor, dual source), a program
  that does not build. Logged once; that shape stays D3D11.
- A failed anchor epoch (`onFailed`) or a batch that did not run returns every effect to D3D11 for the session. A batch whose run
  fails after its draws were withheld loses those draws for that one frame; the pass is never taken again.

## Switches and log lines

| Switch | Effect |
| --- | --- |
| `CS_ORG_IMAGESPACE=1` | Installs the hooks and takes the covered draws. Needs `CS_ORG_ENGINE_CONSTANTS=1`. |
| `CS_ORG_IMAGESPACE_EFFECTS=<list>` | Comma list of engine names (`BSImagespaceShaderHDRDownSample4`), effect names (`ISHDRDownSample4`) or groups (`hdr`, `bloom`, `composite`), any case. Unset or `all`: every covered effect. |
| `CS_ORG_IMAGESPACE_LOG=1` | Logs each new shape. |

Covered effect names: ISHDRDownSample16, ISHDRDownSample4, ISHDRDownSample16Lum, ISHDRDownSample4RGB2Lum,
ISHDRDownSample4LumClamp, ISHDRDownSample16LumClamp, ISHDRDownSample4LightAdapt, ISHDRDownSample16LightAdapt (hdr);
ISBrightPassBlur3 to ISBrightPassBlur15 (bloom); ISCompositeVolumetricLighting, ISCompositeLensFlare,
ISCompositeLensFlareVolumetricLighting (composite).

Log lines (all `[ORG] ImageSpace:`): `hooks installed (...)`; `on (CS_ORG_IMAGESPACE); effects <list>; ...`; `built <effect> for
format <n>`; `registered <n> passes (registration <k>)`; `the graph holds <n> passes; their draws are the graph's from now`; `the
first batch (<n> draws) ran on the graph`; `<effect> stays on D3D11 (<reason>)`; errors: `... every ImageSpace effect returns to
D3D11`.

## Test plan

Scenarios `tools/cs-ab/scenarios/imagespace-org0.env` (D3D11, the capture on) and `imagespace-org1.env` (every covered effect):
Whiterun by day and at night, the world map, the journal (menu blur), Dragonsreach, Riverwood. Bisection:
`imagespace-org1-hdr.env`, `-bloom.env`, `-composite.env`, or any effect name in `CS_ORG_IMAGESPACE_EFFECTS`.

1. The log shows the startup lines, `registered`, `the graph holds` and `the first batch`; no ImageSpace error, no `Render graph
   disabled`, no `[DCLF] Forced off`.
2. Shots match imagespace-org0's: exposure (luminance and adaptation) and bloom by day and at night, VL in Whiterun's day.
3. Refusals are expected for the tonemap (not covered), and for effects inside menus with a rectangle.
4. With `CS_ORG_IMAGESPACE_LOG=1`, the shapes stop growing after the first window; Dragonsreach (a cell change) may add a few.

## What is left

Everything marked Next above: the compute form (VL and SAO compute, the downsample CS, CopySubRegion), the tonemap when no CS
feature owns it, the rest of 99023's slots (DOF, radial and full-screen blur, get hit, refraction, FXAA, world map), the
pre-tonemap SIL and IB lens flares, SSAO, the local map, the copies, and the dynamic-resolution path. Then the engine's draw can
be withheld without the D3D11 bind (P5.13).
