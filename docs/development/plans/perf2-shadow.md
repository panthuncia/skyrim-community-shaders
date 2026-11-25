# Round 2 lead `shadow`: native shadow masks

Branch `perf2/shadow` (CS), based on `org_integration` 0d9dce3ee. No ORG, BasicRHI or SceneWriterFramework change.

## What changed

The shadow mask family no longer captures the engine's draws. At the world's and a menu scene's shadow mask calls
(`Main::RenderShadowMasks` +0x7E / SE +0x90, `UI3DSceneManager::Render` +0x461 / SE +0x466) the family:

1. ends the engine's open technique (FUN_1414f4350, AE 107653 / SE 100860) and sets the call's targets: no depth target,
   kSHADOW_MASK as RT0 with a clear and its viewport (AE 77454 and 77453 / SE 75647 and 75646);
2. computes one draw per light of the view's caster array (`ShadowSceneNode::shadowLightsAccum`, walked as the engine walks it,
   `slot += shadowMapCount`) in `ShadowMaskNative.cpp`, following `BSUtilityShader::SetupTechnique` (AE 0x1414fa160 / SE
   0x14130df90), `SetupGeometry` (0x1414fae40 / 0x14130ec70), the mask textures (0x1414fbab0 / 0x14130f960) and projections
   (0x1414fbd30 / 0x14130fbe0), and the descriptor functions (0x1415221e0, 0x141522260 / 0x141334900, 0x141334970):
   - technique by light kind (directional, frustum with or without focus, omni DPB, else PB), pixel descriptor with the filter bits;
   - PS b0: VPOSOffset (kSHADOW_MASK's size), ShadowSampleParam.zw (Poisson radius / map resolution for filters 2 and 3),
     End/StartSplitDistances (the sun light's splits as depths through RenderShadowMasks' camera, or a local light's slice);
   - PS b2: ShadowMapProj (each map's light transform less the posAdjust, transposed; 4x3 per cascade or one 4x4),
     AlphaTestRef.yz (the light's bias scale; SE multiplies by 1/3 where AE has the product), ShadowLightParam.xz;
   - write mode from the view's table by mask channel (AE 254901 / SE 304858, menu AE 216241 / SE 270481), depth bias 1,
     scissor 1 for a local light, the rest of the shadow state as at the call;
   - t2 the main depth's SRV, t4 the light's map (`depthStencils[descriptor.renderTarget]`), s3 (address 0, anisotropic) when
     the filter has bits, as the D3D11 draw reads it; s4 comparison;
3. applies the pending state (the clear lands before the anchor's run), gives the sun's cascade map to `State::OnShadowMaskDraw`,
   submits the run to the existing client;
4. does the call's tail: write mode back to 1, `ReturnShadowmaps` per light (world), `RestoreClearColors` (menu, AE 77250 / SE 75464).

The engine's call does not run. A batch the native build cannot compute (no screen quad, a light with no map, a texture that
cannot be imported, more than 32 lights) runs the engine's call and is logged once per reason
(`[DCLF] shadow mask: the engine draws: ...`). A draw whose program is still building is left out (logged once), as the sky does.
Program keys are cached by their inputs (state objects, technique, filter bits, format).

Deleted: `ShadowMaskCapture.cpp` (the DrawTriShape taker, the SetupGeometry watcher, the latch), `SlotRead`/`SamplerRead`.
New engine addresses (all as AE/SE pairs, checked in Ghidra): the node AE 415033 / SE 528088, the slice 415266 / 528313, the
camera 414580 / 527667, scene shadows 390933 / 513204, filter mode 390942 / 513194, filter bits 390941 / 513193, Poisson radius
391759 / 513773, map resolution 415268 / 528318, far parameter 415264 / 528316, bias factors 391869 / 513809, 391716 / 513749,
415265 / 528317, render mode 414601 / 527651. Read-only constants (4, 1e8, 0.00025, 8.333e-5, 1/3) are written as literals.
No engine write is hooked, so nothing goes to SceneWriterFramework.

## Parity (before the capture was removed)

A temporary compare build (not committed) built the native run beside the capture at every batch and compared every field the
ORG build reads: key (descriptor, blend, raster, format), PS b0/b2 rows, alpha reference, geometry, buffers, t2/t4, s2-s4,
viewport. Guest run with the river at 13:00, the inventory (menu masks) and Dragonsreach (local lights):

- first run: write mode wrong (the table writes `alphaBlendWriteMode`, not the blend mode), s3 differed, the cascades' translation
  column differed by up to 9.5e-7 (summation order);
- after the fixes: 1800 of 1800 batches equal (largest relative difference 1.5e-7, within tolerance 2e-6).

Shots against the capture build: mean differences 2.0 (river), 1.4 (inventory), 4.0 (Dragonsreach), walking NPCs and adaptation
(the capture build's own run-to-run level). kSHADOW_MASK readbacks (`CS_ORG_SHOT_TARGET=18`, native Vulkan): the sun's mask in red
with the player's and trees' shadows at the river; Dragonsreach's lights in their channels with the pillars' shadows.

## Measured (Dev-Fast, guest+quick frozen river, one light: the sun)

| | baseline (base2) | after (after4) |
|---|---|---|
| window | 3.657 ms | 3.595 ms |
| `Main::RenderShadowMasks` inclusive | 0.098 | 0.064 |
| engine self in `RenderShadowMasksDraw` | 0.033 | 0.004 |
| family under it | 0.063 (`Family.DrawTriShape`, 0.017 of it the constants capture) | 0.058 (`ShadowMask.Native`) |

What the native path costs now: build 0.022 (texture slots 0.005, geometry lease 0.003), submit (the anchor's run) 0.015, the
clear's pending state 0.014. The first baseline capture (6.7 ms window) was taken under load and is not used.

## Shadow maps and caster lights: not done

Where it stands:
- `Main::RenderShadowMaps` (AE 107133) queues the main scene's register jobs (its `JobList::Finish` waits for those, not for
  shadows), then calls each caster light's `Render`. `BSShadowDirectionalLight::Render` (AE 0x141511d60) writes each
  descriptor's target and slice and calls `RenderShadowmap` (0x1414f0cf0), which drops the scene accumulate array, assigns free
  slices for target -1, clears, submits the accumulator and then writes the descriptor's light transform from its camera. The
  shadow masks read those targets, slices and transforms, so skipping `Render` needs that bookkeeping ported first.
- `CalculateAndDrawShadowCasterLights` stays: DCLF lists its shadow candidates after it, and its sun accumulation is already
  skipped where DCLF draws (SunAccumulation). Its `JobList::Finish` (0.039) waits for the selection's jobs.
- Next step: port `RenderShadowmap`'s descriptor bookkeeping (targets, slices, light transforms) and the accumulate-array release,
  then skip `Render` for lights whose views DCLF draws whole, with a parity build comparing the descriptors.

## Tests (final build)

| Config | Label | Result |
|---|---|---|
| guest+quick | p2-shadow-f-guest-quick | passed |
| native-vulkan+quick | p2-shadow-f-nvk-quick | passed |
| native-d3d12-quick | p2-shadow-f-d3d12-quick | passed |
| guest-interior | p2-shadow-f-guest-interior | passed |
| guest, river noon + inventory + Dragonsreach | p2-shadow-f-guest-shadows | passed, no refusals |
| native-vulkan, same, `CS_ORG_SHOT_TARGET=18` | p2-shadow-f-nvk-shadows | passed |

## Decided by the agent, to confirm

- s3 is (address 0, anisotropic) when the filter has bits: the engine leaves it unset then, and that is what the D3D11 draw and
  the capture read. The ORG build always takes SHADOWFILTER 0 (selector 3 is unsupported), so it samples the map through s3.
- FocusShadowMapProj and FocusShadowFadeParam are not computed: the ORG build keeps the stencil at 0 and never reads them.
  Turning focus shadows on in Utility.hlsl needs them ported (0x1414fbd30's second loop, SetupTechnique's fade loop).
- The menu view is native too (same SetupGeometry; its masks matched in the compare build).
- SE and AE 1.7.104 take the same path. Untested on SE at runtime (the game folder is AE 1.6.1170); 1.7.104 uses the AE ids.
