# P5.13 residue: the shadow mask passes on the render graph

The engine's shadow mask passes, the residue census's `RT kSHADOW_MASK DS none` classes (1-2.5 draws a frame in the world, about 1.7 in
the inventory; [flush.md](flush.md), "DCLF's share"), drawn by ORG through Drawcall Limit Fix's engine draw family helpers
(`Core/Engine/Family`, render-graph.md "Shared helpers for engine families"). The code is
`src/Features/DrawcallLimitFix/Features/ShadowMask/` and `package/Shaders/Utility.hlsl`'s ORG build; the switch is `CS_ORG_SHADOW_MASK=1`
(default off). RE done in Ghidra on 2026-11-11, SE 1.5.97 and AE 1.6.1170; AE 1.7.104 is not checked. IDs are Address Library IDs.

## RE

### The functions

| Function | SE | AE | Called from |
| --- | --- | --- | --- |
| Main::RenderShadowMasks (`FUN_1414cd600` on AE) | 100422 / `0x1412e3ac0` | 107140 / `0x1414cd600` | Main::Draw (AE `0x14064484c`), right after Main::RenderDepth (107139) |
| The world's shadow masks (`FUN_1414cd6a0` on AE) | 100423 / `0x1412e3b80` | 107141 / `0x1414cd6a0` | RenderShadowMasks, SE +0x90, AE +0x7E |
| A menu scene's shadow masks (`FUN_140972f70` on AE) | 51856 / `0x1408d2d70` | 52728 / `0x140972f70` | UI3DSceneManager::Render (51855 / 52727), SE +0x466, AE +0x461 |

**RenderShadowMasks** runs only when a light casts shadows (`_gLastFrameEnabledShadowCasterLightCount > 0`, or a flag): it sets the
clear colour (1, 0, 0, 0), or 0 in an interior, `SetCameraData` with the world camera, calls the world's masks, and restores the clear
colour.

**The world's masks**, in order:
1. `SetCurrentDepthStencilTarget(none)`, `SetRenderTarget(0, 0x12 kSHADOW_MASK, CLEAR)`: a deferred clear, issued at the next
   SetDirtyStates or ApplyState (skeleton.md, "What the engine does").
2. For each light of the shadow scene node's caster array (`ShadowSceneNode::GetShadowCasterLightArrayEntry`): a BSUtilityShader render
   pass on **ImageSpaceManager +0x48**, the screen quad TriShape, with the technique by the light's kind: directional (vfunc +0x30)
   `(cascades + 0x20) << 8 | 0x200002` (RENDER_SHADOWMASK, RENDER_DEPTH, TEXTURE; the cascade count in the filter bits), spot (+0x28)
   `0x400100` or `0x400000` (RENDER_SHADOWMASKSPOT, with or without the focus shadows' LOD_LANDSCAPE bit), parabolic (+0x40) `0x1000000`
   (DPB) else `0x800000` (PB), each `| 0x2002` and `+ 0x2B`. The alpha blend mode is the light's (a table indexed by light +0x520,
   AE `0x141aba740`; SE's not read), so the lights' masks combine in the target. `FUN_1414f3dc0` (AE; the pass's immediate
   render) runs BeginPass, SetupTechnique, SetupGeometry and `DrawTriShape` (the census's chain: 77263 <- 107637 +0x1A2 <- 107644 +0x31C
   <- 107141 +0x1C4 or +0x346).
3. The alpha blend mode back to 1.

**The menu scene's masks** are the same loop over the menu's own shadow scene node (`menuObjects`' node, UI3DSceneManager +0x30's
caster array at `0x142033068` on AE), with spot, PB and DPB lights only (no directional branch), its own blend table (AE `0x141905680`),
and `RestoreClearColors` at the end. UI3DSceneManager::Render calls it after the colour accumulator's cull and before its stencil and
`SubmitAccumulator` (menus.md, step 5 to 9): the census's `52728+0x1E5` chain, vs `0x1002002`, ps `0x1062002` (DPB) in the inventory.

**The shader** (Utility.hlsl, `RENDER_SHADOWMASK*` with `RENDER_DEPTH`): the vertex stage places the quad's position (`xy`; `z` from
VS b0 or b2, unread: no depth target). The pixel stage reads the scene depth (t2, s2), rebuilds the position with
`FrameBuffer::CameraViewProjInverse` and `DynamicResolutionParams2`, and tests the light's shadow maps (t3 with s3, t4 with the
comparison sampler s4; the focus shadows' stencil t5 and maps t6 with s6 under FOCUS_SHADOW) with PS b0 (VPOSOffset, the cascade
splits, the filter's parameters) and PS b2 (the projections: ShadowMapProj, FocusShadowMapProj, AlphaTestRef, ShadowLightParam), then
discards below the alpha test reference (b11). SharedData gives InInterior and the frame count (the noise).

## The family

The pieces are the water's and the screen blood's (water.md, "The family").

- **The calls** (`ShadowMaskCapture.cpp`, thunks on both calls). Each view's batch runs with its draws latched, the engine's code
  unchanged: the render pass, the Utility shader's SetupTechnique and SetupGeometry and the render state are the engine's.
- **The choke point** (the family's taker at the shared DrawTriShape detour). A latched draw records the shadow state (blend and write
  mode, raster state, viewport), PS b0 (5 rows) and b2 (28 rows) through **ConstantMirror** (watched from a BSUtilityShader::SetupGeometry
  watcher), the alpha test reference (as the sky reads it), t0-t7 and their samplers, and the quad's geometry. Then it **applies the
  engine's pending state** (`Engine::ApplyPendingState`, SetDirtyStates(false)) as DrawTriShape's own would: the function's kSHADOW_MASK
  clear is issued there (on D3D11, or as the engine skeleton's clear pass), before the anchor's run draws the masks, and the dirty bits
  are consumed as before.
- **The pass** (`ShadowMaskDraws.cpp`). After the batch the EngineDraws anchor runs (`Family::RunWithheld`). Each view has a client
  (`cs.shadow-mask.world`, `cs.shadow-mask.menu`), made once, whose pass draws the batch's draws into kSHADOW_MASK (imported once,
  `FrameAnchors::EngineTarget(18)`) in the engine's order, one RasterProgram draw each behind the client's run gate, with Utility.hlsl
  built for ORG: ShaderCache's defines for the pixel descriptor, the pipeline the engine's blend and rasterizer objects describe
  (`Family::EngineDrawPrograms`, built off the render thread: a draw whose program is still building stays the engine's), no depth.
- **Shared constants.** FrameBuffer and SharedData are the client's shared constants (`ClientDesc::sharedConstants`), which the anchor's
  epoch writes at its run from what D3D11 would read then (`frameBufferCached`). The run follows the batch, so a menu scene's run reads
  the menu camera's per-frame buffer, as the engine's draw would.
- **Published slots.** A slot holding a published graph texture's export (a depth or an engine target a graph pass writes) is read by
  the id's whole-resource index (`Family::PublishedSlot`), and the id joins the view's shape (a set of ids, kept once seen): the probes
  learn it, and the pass declares the read so the graph orders the write first. The water does the same (water.md).
- **Per-draw latch** (640 bytes, `DrawConstants` / `UtilityOrgShadowMaskDraw`): the rows, the reference, the slots' indices, the
  geometry. The push block (24 bytes): the shared constants and the latch region.
- **Probes, refusals and the shape** as the water's: a view's batches are the graph's only after a probe found every draw latchable;
  a draw a later batch cannot latch makes the rest of the batch the engine's and the view probes again; reasons logged once
  (`[DCLF] shadow mask: the engine draws: ...`).

### The shader's ORG build

`Utility.hlsl` under `CS_ORG_BINDLESS` (RasterProgram's builds only; DCLF's own Utility builds define `DCLF_BINDLESS` and are unchanged)
builds only the shadow mask techniques (an `#error` for any other): its PS constants, alpha test reference and slots come from the
latch (a column_major matrix rebuilt from its column registers), and the vertex stage pulls the quad's vertex (`DCLFVertexPull.hlsli`,
as Water.hlsl). `verify-shader-refactor.ps1`: IDENTICAL for 14 PS permutations (the four shadow mask techniques with SHADOWFILTER 0, 1, 2,
4 and 8 and FOCUS_SHADOW, depth, shadow map, alpha test, refraction normals, local map, parabolic shadow maps) and 8 VS permutations.
`check-dual-target-shaders.py`: FXC, SPIR-V, DXIL and spirv-val pass for RENDER_SHADOWMASK (filters 1 and 4, FOCUS_SHADOW),
RENDER_SHADOWMASKSPOT, PB and DPB, both stages, push block 24 bytes.

### Runs a frame

EngineDraws now runs up to five times a frame: the world's masks, the water mask, the water, a menu scene's masks, the screen blood.
That is 8 more frame slots, past the old budget of 255 (252 with every switch on): it needs the budget of 512 from `org-port/w3-slots`.
Merge that first. A frame with more runs than that (none is known) gets the engine's draws for the batches past it (`CanRun`).

### Graph rebuilds

- **A client is made once per view**, after its shape held for 30 batches (the ported features' first publications settle). That
  registration is the family's only graph build; with DCLF it happens at DCLF's build point, in a menu-only frame at the frame's end.
- **Nothing registers again during play.** A batch whose published slots differ from its client's stays the engine's (logged once,
  with what changed), D3D11 slots change only a heap index in the latch, and kSHADOW_MASK is imported once: made again (a resolution
  change), the view stays the engine's until the next start.

## The switch

`CS_ORG_SHADOW_MASK=1` (render-graph.md, "Switches"); default off; needs DCLF running. `CS_ORG_SHADOW_MASK_VIEWS=world`, `menu` (or
both, comma separated; the default) keeps the other view on the engine.

## Test plan

1. Build (with `org-port/w3-slots` merged), then `shadow-mask-org0.env` and `shadow-mask-org1.env` (`tools/cs-ab/scenarios/`):
   Whiterun at 13:00, Dragonsreach, the inventory and the magic menu, with the residue census.
2. Log (org1): `[DCLF] shadow mask: on (CS_ORG_SHADOW_MASK=1)`, `hooks installed`, `the cs.shadow-mask.world client registered`,
   `built the program for descriptor ...`, `every world shadow mask draw can be drawn by the graph`, `the first cs.shadow-mask.world run
   drawn by the graph`, and the same for `menu` in the inventory. No `back to the engine's draws`, no `a withheld batch was not drawn`,
   no `the engine draws` after the first frames, no `SPIR-V build of Utility.hlsl` error, no `[E]` line but the known EFFECTS11 one.
3. The census windows lose `RT kSHADOW_MASK DS none` and the draws `<- ... (ID 107141+0x1c4)`, `+0x346` and `(ID 52728+0x1e5)`.
4. Shots against org0's: the sun's shadows in Whiterun (cascades, their blend), Dragonsreach's point and spot light shadows, the item's
   shadow in the inventory. A mask never cleared shows last frame's shadows smeared; a wrong matrix shows shadows offset or missing.

Not verified without a build: everything above. Risks to look at first: the column_major rebuild of ShadowMapProj, whether the quad's
vertex stride passes `LatchGeometry` (16 bytes at least), the comparison sampler (filter mode 4) through `GpuTextures::SamplerBinding`,
the stencil SRV at t5 (FOCUS_SHADOW) through `GpuTextures`, and the alpha test reference when the engine leaves the test disabled.
