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

Still refused (each needs work outside this lane's files):
- **A scissor, or a depth or stencil test.** `AnchorPasses::Draw` has no scissor and no depth-stencil attachment. `RasterProgram`
  already takes a depth format and state. Menu 3D's stencil `NOT_EQUAL` composite and its colour fill need it. Noted in
  `org-extension-api-notes.md`.
- **A viewport outside its target.** The render area is the viewport (`RasterProgram::Viewport`). To clip it, TEXCOORD0 would have
  to come from the full viewport, which needs a separate dynamic viewport in BasicRHI.
- **A blend state outside the engine's table.** No case is known. The table is read at draws and builds, and the manifest keeps it.
- **A manager rectangle (menu 3D) and the dynamic-resolution quad.** Kept. It is not checked in Ghidra whether 75564 writes the
  rectangle into the shadow state's viewport (if it does, `ReadViewport` already reads it and the refusal could go).

## Effects

| Effect | State | How to trigger it in game | What to look for |
| --- | --- | --- | --- |
| ISTemporalAA | Done. A two-target pass (the image and the next history), from ISTemporalAA.hlsl's new ORG build. Each call is a scope of its own. | Native mode (Upscaling off), or guest with Upscaling off and the game's TAA on (`bUseTAA=1`). | No `ISTemporalAA` in the 300-frame line. A refusal names t5 (alpha) if it is not an engine target. History ping-pong: the second direction is built on first sight in the first session. |
| ISTemporalAA_UI, ISTemporalAA_Water | Not done. CS's ISTemporalAA.hlsl has no `TAA_UI` or `TAA_BLEND_WATER` path, so both stay on vanilla DXBC (imagespace-shaders.md). | A menu over the world with TAA on; `coc` to a cell with water, then look at it. | `... into rt.. (not covered)` in the 300-frame line. |
| ISCopyScaleBias, ISCopyTextureMask | Done, through ISCopy.hlsl's ORG vertex stage (the fullscreen triangle with the same affine change). | Menus and the loading screen (scaled copies); the race menu (texture mask). | No `ISCopyScaleBias` / `ISCopyTextureMask` in the summary. |
| Copies from `kFRAMEBUFFER` | Done when the engine has an SRV of the back buffer. | Take a screenshot (PrintScreen); the journal over the world. | `ISCopy into rt..` gone from the summary. A refusal `an input that is not an engine target` means the back buffer has no SRV. |
| ISLocalMap | Done (ISLocalMap.hlsl dual-target, its own scope). | Open the map, then the local map. | No `ISLocalMap` in the summary. |
| ISMap, ISNoiseScrollAndBlend, ISNoiseNormalmap | Taken with no pass. Their vertex shaders write POSITION, not SV_Position, so the engine's draw rasterizes nothing (inferred; imagespace-shaders.md). The engine's state is still applied. | The map menu (ISMap). | They leave the census. If the map looks different from D3D11, the inference is wrong. |
| ISSimpleColor | Done without a stencil test. With menu 3D's stencil, still refused. | Inventory or race menu (menu 3D). | `ISSimpleColor stays with the engine: a depth or stencil test` in menu 3D. |
| ISSILComposite | Done (composites' scope). | Needs SIL on (game setting). | |
| ISSnowSSS | Done (its own scope). | Snowy exterior, e.g. `coc Winterhold` or `coc HighHrothgar`. | |
| ISAlphaBlend | Not done. Its 16 tint masks are loaded textures, not engine targets; the latch holds one source by heap index. | Race menu (face tints). | `ISAlphaBlend ... (not covered)`. |
| ISUnderwaterMask | Not done. Its source is Water.hlsl's UNDERWATERMASK form, a large shared file. | `coc` into a cell with water, then swim under it. | |
| FXAA, raster and IB lens flares, the dynamic-resolution upsample, ISSILRawInd | Not done. The sources exist. Each needs an ORG resource list and a row; the upsample also needs the dynamic-resolution refusal lifted. | FXAA with the game's FXAA setting; lens flares with a lens-flare weather by day. | |
| The p5 "Next" rows: ISApplyReflections, the parallax-mask blur CS, ISLightingComposite x3, Perlin noise CS, the DOF downsample (58) and raster blur (11) | Not done in this lane. | | |
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
