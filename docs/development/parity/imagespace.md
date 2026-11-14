# Parity wave: ImageSpace (`org-native/par-imagespace`)

Goal: no ImageSpace draw is refused in any window (world, menus, map, loading screens). In native mode, and in guest with the flush
latch on, a draw the port refuses is lost. Code: `src/RenderGraph/Engine/ImageSpace/ImageSpace.cpp`. Design and history:
[p5/imagespace.md](../p5/imagespace.md). Switches as before: `CS_ORG_IMAGESPACE=1` with `CS_ORG_ENGINE_CONSTANTS=1`.

Every shader change below is its own `refactor(shaders)` commit. The ORG code sits under `CS_ORG_BINDLESS`, and
`tools/verify-shader-refactor.ps1` shows the D3D11 builds are byte-identical (PS, and VS where the file has one).
`tools/check-dual-target-shaders.py` builds each ORG form for FXC, SPIR-V (spirv-val passes) and DXIL.

## Refusals removed

| Refusal | Now | Log |
| --- | --- | --- |
| A shape this session did not register (manifest miss) | Built on first sight: a client of its own (`cs.engine.imagespace.late.<n>`) with one pass. `FrameAnchors::Register` asks the build manager, which builds the anchor's graph off the render thread. The main registration keeps running. Once that graph is attached, each draw of the shape is a run of its own, after the main batch. Draws made before then stay with the engine (a frame or two). On a resize these shapes join the main registration. At most 64 per session. | `<effect> into <target> is a shape this session did not register; its pass is built now off the render thread (...)`, then `<pass> is drawn on the graph from now (built on first sight)`. Meanwhile the refusal reads `a shape first seen this session, whose pass is being built off the render thread`. |
| A scope that used its runs this frame | Gone. Run leases give the anchor as many runs as it makes. `kScopeRuns` is deleted. | none |
| A clear at the bind | The pending batch runs first. `SetDirtyStates` makes the clear through the D3D11 context, in stream order. The draw then starts a new batch. | none |
| A copy from `kFRAMEBUFFER` | The back buffer's SRV is an input like any target. `EngineTarget(0)` imports it (in native mode, the graph-owned slot), so the pass declares the read. | none |

Round 1 still refused a scissor, a depth or stencil test, a viewport outside its target and the manager rectangle. Round 2 takes
them (below). The dynamic-resolution quad itself is still refused, and a blend state outside the engine's table (no case known).

## Effects

| Effect | State | How to trigger it in game | What to look for |
| --- | --- | --- | --- |
| ISTemporalAA | Done. A two-target pass (the image and the next history), from ISTemporalAA.hlsl's new ORG build. Each call is a scope of its own. | Native mode (Upscaling off), or guest with Upscaling off and the game's TAA on (`bUseTAA=1`). | No `ISTemporalAA` in the 300-frame line. A refusal names t5 (alpha) if it is not an engine target. History ping-pong: both directions are registered at Setup since round 2. |
| ISTemporalAA_UI, ISTemporalAA_Water | Not done. CS's ISTemporalAA.hlsl has no `TAA_UI` or `TAA_BLEND_WATER` path, so both stay on vanilla DXBC (imagespace-shaders.md). | A menu over the world with TAA on; `coc` to a cell with water, then look at it. | `... into rt.. (not covered)` in the 300-frame line. |
| ISCopyScaleBias, ISCopyTextureMask | Done, through ISCopy.hlsl's ORG vertex stage (the fullscreen triangle with the same affine change). | Menus and the loading screen (scaled copies); the race menu (texture mask). | No `ISCopyScaleBias` / `ISCopyTextureMask` in the summary. |
| Copies from `kFRAMEBUFFER` | Done when the engine has an SRV of the back buffer. | Take a screenshot (PrintScreen); the journal over the world. | `ISCopy into rt..` gone from the summary. A refusal `an input that is not an engine target` means the back buffer has no SRV. |
| ISLocalMap | Done (ISLocalMap.hlsl dual-target, its own scope). | Open the map, then the local map. | No `ISLocalMap` in the summary. |
| ISMap, ISNoiseScrollAndBlend, ISNoiseNormalmap | Taken with no pass. Their vertex shaders write POSITION, not SV_Position, so the engine's draw rasterizes nothing (inferred; imagespace-shaders.md). The engine's state is still applied. | The map menu (ISMap). | They leave the census. If the map looks different from D3D11, the inference is wrong. |
| ISSimpleColor | Done, with menu 3D's stencil test and scissor since round 2. | Inventory or race menu (menu 3D). | No `ISSimpleColor` in the summary in menu 3D. |
| ISSILComposite | Done (composites' scope). | Needs SIL on (game setting). | |
| ISSnowSSS | Done (its own scope). | Snowy exterior, e.g. `coc Winterhold` or `coc HighHrothgar`. | |
| ISAlphaBlend | Done (round 2): the 16 tint masks by heap index from the latch. | Race menu (face tints). | No `ISAlphaBlend` in the summary. |
| ISUnderwaterMask | Done (round 2): Water.hlsl's UNDERWATERMASK ORG form, a geometry effect. | `coc` into a cell with water, then swim under it. | |
| FXAA, raster and IB lens flares, the dynamic-resolution upsample, ISSILRawInd | Round 2: all done except the raster lens flares (BSLensFlare). | FXAA with the game's FXAA setting; lens flares with a lens-flare weather by day. | |
| The p5 "Next" rows: ISApplyReflections, the parallax-mask blur CS, ISLightingComposite x3, Perlin noise CS, the DOF downsample (58) and raster blur (11) | Round 2: the apply, the parallax blur and the lighting composite done; 58 and 11 were rows already; Perlin noise left. | | |
| Sun glare 0x5C006072 | Sky family's, not this lane. | | |

## How to test

1. Run once to fill the manifest. The second run judges, but a shape missed in run one should now be built on first sight.
2. Native Vulkan: Whiterun, Dragonsreach, the map menu (world and local map), the race menu, a screenshot, a snowy cell. Check the
   `draws and dispatches left to D3D11` line every 300 frames. Under the latch the census should drop by the rows above.
3. Look for `[ORG] ImageSpace:` errors: `failed with the anchor`, `could not be made`, `has no program`.
4. Shots against D3D11: TAA stability (no ghosting or jitter), copies in menus, the local map's edges, the map.

## Not verified without a build

- The C++ is not compiled. Check: `LateClient`'s `std::atomic<bool>` with `make_shared`, the `Admission` struct, and the
  positional initializers of the new `kEffects` rows (16 fields).
- That `FrameAnchors::Register` of a second client keeps the main client's `CanRun` true while the build is pending (it should: the
  main registration's serial is already built).
- TAA: the engine's TAA constants use CS's member names, which the doc says may all land at c0 in CS's constant table. The ORG
  build reads the bytes the engine wrote, so it matches the D3D11 build either way.

## Round 2 (`org-native/par-imagespace2`)

### AnchorPasses and RasterProgram

`AnchorPasses::Draw` takes a depth-stencil attachment (`depth`, declared `DepthRead`, or `DepthReadWrite` when the program's depth
state writes), a `stencilReference`, a `scissor` and a `dynamicViewport`. `RasterProgram::DrawDesc` sets the last two inside the pass
(BasicRHI `SetViewports` / `SetScissors` after `BeginPass`): the render area clips them, and a fullscreen draw's TEXCOORD0 spans the
draw's viewport. `RasterProgram.{h,cpp}` got this small addition; no other caller changes.

### Refusals removed

| Refusal | Now |
| --- | --- |
| A depth or stencil test | The draw's depth target (`ShapeKey::depth`, `depthMode`, `stencilMode`, `stencilRef`) is the pass's depth attachment, imported with `EngineDepth`, drawn through its DSV's format, with the engine's depth-stencil state for those modes (read from the table at the draw, kept in the manifest's `depthStencils`). A depth clear at the bind runs the pending batch first, as a colour clear does. Still refused: a slice other than 0, and a draw that reads the depth target it tests against. |
| A scissor rectangle; the manager's rectangle (menu 3D) | Checked in the SE binary: `BSImagespaceShader::Render` (0x14130ce28 to 0x14130ce71) turns the raster state's scissor on and passes the rectangle to 75564, which is `RSSetScissorRects` with (x, y, x + width, y + height) (0x140d70100). It is not written into the shadow state's viewport. A detour on 75564 / 77365 (`Scissor`) keeps the last rectangle; a draw whose raster state scissors takes it into its shape. The `Ineligible` rectangle check is gone. A scissored draw with no rectangle seen this session is refused (`a scissor the engine did not set through its scissor function`). |
| A viewport outside its target, not whole pixels, or a depth range other than 0 to 1 | The shape keeps the viewport as floats (snapped within 1/64 pixel, depth within 1e-3, so dynamic resolution's products give one shape). The render area is its pixels inside the output, rounded outward; when the viewport is not exactly that area it is the draw's `dynamicViewport`. Refused only when nothing of it is inside (`a viewport entirely outside its target`). |
| Dynamic resolution (the manager's flag) | Narrowed: only a draw of the dynamic-resolution quad (+0x50) is refused (`dynamic resolution's quad`). An effect drawn with the full quad while the flag is set (the upsample) is taken. |

Manifest version 2 (the key changed): the first session after this build starts an empty manifest, so its shapes are built on first
sight and written for the next.

### First-build samplers

Samplers come from the shape's address and filter modes alone (`EngineSamplerDesc`), as the renderer builds its table (SE 75831 at
0x140d89480, read in the binary: clamp, U clamp V wrap, U wrap V clamp, wrap, border, mirror; point and linear without mips, trilinear,
anisotropic at CS's cap of 8, the LESS comparison sampler; MinLOD -FLT_MAX). No pass waits for the engine's table, so the ~86 lines
`sampler 0 (address N, filter M) has no ORG form at the graph's build` are gone, and `RetryBuild` no longer waits on samplers. A
sampler the backend cannot make is now an error (`could not be made`).

### TAA history

`AddImpliedShapes` adds the swap of each TAA history pair (`kTEMPORAL_AA_ACCUMULATION_1/2`, the UI pair, the water pair) to every
shape that mentions one, after the kMAIN / kMAIN_COPY swap. Both directions are registered at Setup.

### Effects added

Each shader is a `refactor(shaders)` commit: `verify-shader-refactor.ps1 -BaseRef HEAD` shows the D3D11 builds identical (the
permutations named), and `check-dual-target-shaders.py` builds FXC, SPIR-V (spirv-val passes) and DXIL.

| Effect | Source, checks | How to trigger in game | What to look for |
| --- | --- | --- | --- |
| ISAlphaBlend | ISAlphaBlend.hlsl; PSHADER and HDR_OUTPUT identical; push 96 bytes. The 16 tint masks are loaded textures: `EffectInfo::latched` (a bit per slot) reads them by heap index from a second latch region (`OrgEngineSourcesAt`; the latch stride is now 96 bytes). A latched slot holding an engine target is refused. | Race menu, change face tints. | No `ISAlphaBlend` in the 300-frame summary; tint colours as on D3D11. |
| ISApplyReflections | identical; push 48. | SSR on, a wet or reflective exterior. | |
| ISLightingComposite (3 forms) | identical in the plain, DIRECTIONALLIGHT and MENU builds; push 88. Not known to be drawn by a vanilla frame. | | |
| ISUpsampleDynamicResolution | identical; push 32. | Vanilla dynamic resolution on (`bEnableAutoDynamicResolution=1`, Upscaling off). | |
| ISParallaxMaskBlurHCS/VCS | ISBlurCS's existing ORG build; the parallax-mask defines build. Rows only. | | |
| ISDownsample, ISBlur, ISIBLensFlares | ISIBLensFlare.hlsl identical; push 32. ISDownsample and ISBlur's existing ORG builds (their IB flare defines build). | IB lens flares on (the game's lens flare setting), look at the sun. | |
| ISSILRawInd | identical; push 64. In the composites' scope. | SIL on. | |
| ISFXAA | PSHADER, HDR_OUTPUT and VSHADER identical; push 32. The ORG entry takes no TEXCOORD1 (the graph's triangle does not write it; never read). | The game's FXAA on (`bFXAAEnabled=1`), TAA off. | No `ISFXAA` in the summary. |
| ISUnderwaterMask | Water.hlsl's UNDERWATERMASK form: the vertex stage pulls the engine's geometry by device address (as the ripple makers do); VS and PS identical. The DXIL builds use no push block, which the check tool flags (the D3D12 vertex stage draws nothing, the pixel stage reads nothing). Every draw of a geometry effect's call is taken now, not only the first. | Swim under water. | The underwater tint where expected; no `ISUnderwaterMask` in the summary. |

The DOF downsample (58, ISHDRDownSample4) and the raster blur (effect 11, ISBlur7) were rows already (own scopes); their shapes in
the DOF chain are built on first sight if the manifest lacks them.

### Not verified without a build

- The C++ is not compiled: `RasterProgram::DrawDesc`'s new members in designated initializers (order: viewport, dynamicViewport,
  scissor, constants), `PassBuilder::DepthRead` on a `FrameAnchors::Texture`, `std::popcount`, `std::bit_cast`, the 17-field
  `kEffects` rows, `Pending`'s optional `SourcesAt`, the `Scissor` detour on 75564 beside LegacyGraphicsCompatibility's two call
  patches (they call the same entry, which now jumps to the detour).
- Ghidra was down (connection refused); the RE above is from the unpacked SE binary, disassembled with capstone. AE (77365, 77653) is
  assumed the same, as CS's other uses of these functions assume.
- That menu 3D's depth target imports with `EngineDepth`, and that DepthRead with a stencil test and a loaded and stored stencil is
  accepted by both backends.

### Left

- **ISTemporalAA_UI and _Water:** CS's ISTemporalAA.hlsl has neither path. Writing them needs vanilla's DXBC for both (the UI build
  reads the mask from t4, the water build blends a water history) to transcribe and check; not available in this lane.
- **Raster lens flares (BSLensFlare, 100):** sprites drawn with their own geometry, a vertex stage that reads its own PerGeometry
  (VS b2, which the latch does not capture), a loaded sprite texture and the visibility target. Needs VS b2 in the latch and a pulled
  vertex stage; it is also not known that they draw through `BSImagespaceShader::Render`.
- **Perlin noise (128, 129):** the 3D form is Volumetric Lighting's port; its output is not an engine render target.
- **The dynamic-resolution quad:** its vertices are not known, so draws with it stay refused.
- **ISExp (123), the compute downsamples (121, 122), debug (118, 149):** not converted.
