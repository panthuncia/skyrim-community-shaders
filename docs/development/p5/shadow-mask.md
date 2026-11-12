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
  the id's whole-resource index, and the pass declares the read so the graph orders the write first. The ids are fixed when the client
  is made (`Family::PublishedInputs`, `ListInputs` in `ShadowMaskDraws.cpp`: the engine depths `Engine::Depth::kMAIN`, `kMAIN_COPY`,
  `kPOST_ZPREPASS_COPY`, `kSHADOWMAPS`, `kSHADOWMAPS_ESRAM`, `kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM`, `kFOCUS_NEO`, and the ids DCLF's
  registry lists at t17), declared at each build while registered, and read by index once a build declared them. Until then, and for
  an id not listed, the batch stays the engine's, logged once per id (`a world draw's t3 holds '<id>', ...`). The water does the same
  (water.md, "Published inputs"). Terrain Blending binds its blended depth at t2, so `TerrainBlending::BlendedDepth` is listed too
  (p5-all-census-misc showed `a world draw's t2 holds 'TerrainBlending::BlendedDepth'` before it was).
- **Slots read.** Utility.hlsl's ORG build reads only t2, t3 and t4 (`SlotRead`). t0, t1, t5, t6 and t7 are latched as null views
  whatever is bound, so they can never refuse a draw: t0 usually still holds the last material's texture, often a streamed one
  that is not importable yet; t5 and t6 are the focus shadows' (below).
- **Per-draw latch** (640 bytes, `DrawConstants` / `UtilityOrgShadowMaskDraw`): the rows, the reference, the slots' indices, the
  geometry. The push block (24 bytes): the shared constants and the latch region.
- **Setup, withholding, running** (primary path, primary-path.md). The client is made at CS's setup (`Family::RunSetups`,
  before the setup's graph build), so it is in its anchor's graph before any world frame. A batch is taken when the client's targets match and `FrameAnchors::CanRun` says
  the anchor's run will draw it; otherwise the engine draws the batch, vouched for (`Flush::EngineDraws`), logged once. After the
  batch (or at a refusal) `Family::RunWithheld` runs the anchor; any status but `Ran` is an error, logged once, and nothing is
  retried. The client's `executed` callback counts the runs that drew.
- **Refusals end the latched part.** There is no probe: every batch is latched from the first. A draw the family cannot take (its
  inherent refusals, "What stays the engine's") runs the draws latched so far at that point and leaves the rest of the batch to the
  engine, vouched for (`Flush::EngineDraws` until the batch returns), so the engine's draws come after the graph's in the engine's
  order. A pass the dispatcher draws through another function than DrawTriShape
  (`Family::DrawsThroughTriShape`) is refused at its SetupGeometry, before the engine draws it. A batch that ran nothing
  keeps its imports (`Keep`).
- **Lasting refusals.** Those reasons are logged the first time only, so a refusal that never clears used to go quiet. A view that
  stays the engine's for 120 batches in a row now says why, once per view and reason (`[DCLF] shadow mask: a view stays the
  engine's: ...`): either `its world client is not in the EngineDraws anchor's built graph` (not made, or still joining), or
  `every world probe finds a draw the graph cannot take: <reason> (<slot or geometry detail>)`. A missing render graph host when
  the client is due is no longer a permanent refusal: the next batch asks again.

### The focus shadows' stencil (t5)

The world's sun mask and the spot lights' masks are FOCUS_SHADOW techniques when an actor has a focus shadow (CS's descriptor bit
0x100 with RENDER_SHADOWMASK or RENDER_SHADOWMASKSPOT: `0x262102` for the sun, `0x462102` for a spot light). Their shader loads
t5, the main depth's stencil view (DXGI 47, X24_TYPELESS_G8_UINT), and takes the focus shadow maps (t6) where the stencil is not 0.

- **Why the world never moved.** GpuTextures rejects that view (`rhi::helpers::ToRHI` has no X24_TYPELESS_G8_UINT, and BasicRHI's
  Vulkan views of a depth-stencil format always take the depth aspect: `VkSampledImageAspectMaskForFormat`). Every world probe
  failed on it from the first sun mask on, and the refusal was logged once (`a world draw's texture t5 could not be imported, t5:
  format 47 ...`), then never again: the view stayed on the engine with no further line. The menu's DPB lights are no
  FOCUS_SHADOW technique, but the stencil view was often still bound there, so a menu batch was refused the same way now and then.
- **What the engine's draw reads.** Utility.hlsl reads the stencil from the view's red component (`.x`). DXVK's stencil view puts
  the stencil in green and zero in red (`d3d11_view_srv.cpp`: "Shaders expect the stencil value in the G component", swizzle
  ZERO, R, ZERO, ZERO). So on this DXVK build the engine's draw reads 0 and never takes the focus shadows' branch.
- **The ORG build** keeps the stencil value 0 without a view (`!defined(UTILITY_ORG)` around the load), the same as the engine's
  draw, and t5 and t6 are latched as null views. No stencil import is needed.
- **If Utility.hlsl is changed to read `.y`** (focus shadows on), the ORG build needs the stencil too, and BasicRHI cannot give it
  a view: org-extension-api-notes.md, "A stencil view of a depth-stencil image". Until then the only way would be a D3D11 copy
  of the stencil into an R8_UINT texture the graph imports.

### The shadow map at t3 and t4

The shadow mask descriptors carry filter selector 3 (bits 17 and 18), which ShaderCache does not support: it logs
`Unsupported Utility shadow-filter selector 0x3` and builds SHADOWFILTER 0 for both builds (dclf-validation.md, "Bugs found by
parity"). SHADOWFILTER 0 samples the shadow map through its plain view, t3, and compares by hand; the other filters use the
comparison view, t4.

- **Why the world's sun masks came out black.** Latched from t3, the sun's masks read a null view (2026-11-11, a probe of the
  bound views' sizes: t3 0 wide, t4 the cascade array, about 4096 wide with 2 slices). Every sample was 0, below every compare
  value, so every pixel was in shadow, and with the masks on the graph the sun lit nothing.
- **The ORG build** reads the plain view through t4's index (`TexShadowMapSampler` is `Textures[1].x`): t3 and t4 are views of the
  same shadow map, and every light type samples both at the same slice. t3 is no longer a read slot (`SlotRead`); its sampler, s3,
  still is (`SamplerRead`), since the filter-0 path samples through it.
- `shadow-mask-org1.env` against `shadow-mask-org0.env` after the change: mean diffs 2.2 (Whiterun), 4.7 (Dragonsreach), 2.5 and
  2.5 (inventory, magic), the differences walking NPCs; 16 to 22 before.

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

- **Both clients are made at setup** (the main menu), with every published input listed; the engine depths among them are
  imported then, so the first build declares them.
- **Nothing registers again during play.** The published inputs are a fixed list: a newly published one joins the declared reads at
  the rebuild its first Publish causes for every graph. D3D11 slots change only a heap index in the latch, and kSHADOW_MASK is
  imported once: made again (a resolution change), the view stays the engine's until the next start.

## The switch

`CS_ORG_SHADOW_MASK=1` (render-graph.md, "Switches"); default off; needs DCLF running. `CS_ORG_SHADOW_MASK_VIEWS=world`, `menu` (or
both, comma separated; the default) keeps the other view on the engine.

## Test plan

1. Build (with `org-port/w3-slots` merged), then `shadow-mask-org0.env` and `shadow-mask-org1.env` (`tools/cs-ab/scenarios/`):
   Whiterun at 13:00, Dragonsreach, the inventory and the magic menu, with the residue census.
2. Log (org1): `[DCLF] shadow mask: on (CS_ORG_SHADOW_MASK=1)`, `hooks installed`, `the cs.shadow-mask.world client registered`,
   `built the program for descriptor ...`, `every world shadow mask draw can be drawn by the graph`, `the first cs.shadow-mask.world run
   drawn by the graph`, and the same for `menu` in the inventory. No `back to the engine's draws`, no `a withheld batch was not drawn`,
   no `the engine draws` after the first frames (the first batches' `ConstantMirror has no copy` and `its ORG program is still
   building` are expected), no `a view stays the engine's`, no `texture t0` or `texture t5 could not be imported`, no `SPIR-V
   build of Utility.hlsl` error, no `[E]` line but the known EFFECTS11 one and `Unsupported Utility shadow-filter selector 0x3`
   (below).
3. The census windows lose `RT kSHADOW_MASK DS none` and the draws `<- ... (ID 107141+0x1c4)`, `+0x346` and `(ID 52728+0x1e5)`.
4. Shots against org0's: the sun's shadows in Whiterun (cascades, their blend), Dragonsreach's point and spot light shadows, the item's
   shadow in the inventory. A mask never cleared shows last frame's shadows smeared; a wrong matrix shows shadows offset or missing.

Known, not this family's: ShaderCache logs `Unsupported Utility shadow-filter selector 0x3` for every shadow mask descriptor (bits
17 and 18 of CS's descriptor are set) and builds SHADOWFILTER 0. CS's D3D11 builds take the same defines, so both builds agree.

Not verified without a build: everything above. Risks to look at first: the column_major rebuild of ShadowMapProj, whether the quad's
vertex stride passes `LatchGeometry` (16 bytes at least), the comparison sampler (filter mode 4) through `GpuTextures::SamplerBinding`,
and the alpha test reference when the engine leaves the test disabled.

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

- A view kept off by `CS_ORG_SHADOW_MASK_VIEWS`, and kSHADOW_MASK made again since its client imported it (a resolution change).
- Per draw: not BSUtilityShader, a target other than kSHADOW_MASK alone (or a depth target bound), a state with no description or no
  ORG form, geometry that is not a static triangle-list TriShape within its buffers, a read slot holding a published texture not
  listed (or published during play), a texture that cannot be imported, more than 32 draws, an inactive engine constants capture.

Changed with the primary path: the program's descriptor is CS's lookup (`State::modifiedPixelDescriptor`), and the stages are
compiled for Utility's RenderShadowmask* pixel descriptors only.

### What can trigger a graph rebuild (primary path)

The two clients' registrations at setup. Nothing in play registers again.

### What to look for in the log

- `[DCLF] <family>: the <id> client registered at setup (...)` once per client, at CS's setup, before `graphs built at CS's setup`, and no `frame anchors: cs.<family>.*
  registered` line after it; `; not published yet: <ids>` names inputs no producer had published by then.
- `[DCLF] engine draw programs: the manifest lists N keys`, `[DCLF] <family>: N programs from the manifest queued`, and on a second
  run no `was not prebuilt` line. Each such line names a key the enumeration or the manifest missed.
- `[DCLF] <family>: the engine draws: ...` lines name inherent refusals (once each); `[E]` `latched draws were not drawn this frame`
  and `the engine draws a batch: ... its anchor cannot run now` are errors.
- With the latch on, a batch the family did not take, or the rest of one, reaches D3D11 unvouched: an `[E] [ORG] flush latch: engine
  ... nobody vouched for it` line, counted by the residue census. The `<family>: left to the engine` reasons are deleted.
