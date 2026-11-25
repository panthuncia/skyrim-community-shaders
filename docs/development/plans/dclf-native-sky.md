# DCLF: the sky without draw capture

Status: done. Steps 1-3a 2026-11-24 (batch and cloud layers native), step 4 2026-11-25 (the sun query), step 5 2026-11-25 (the latch
path and `CS_DCLF_SKY_NATIVE` deleted; native is the only path). Owner: the DCLF sky family (`src/Features/DrawcallLimitFix/Features/Sky/`). Background: [p5/sky.md](../p5/sky.md)
(the reverse engineering and the capture path this replaces).

## Goal

The sky family draws the engine's sky without running the engine's per-draw code or latching its draws. Today the batch runs
with its draws latched: for every pass the engine runs BeginPass, SetupTechnique and SetupGeometry, writes constant buffers,
binds textures and calls `DrawTriShape`, and the family reads all of it back (state, constants, textures, geometry) per draw.
That is about 6-8% of the render thread in a Riverwood frame (Dev-Fast).

Instead, the family reads the passes the engine registered for the sky, computes each draw's inputs itself from engine data (the
pass's geometry and property, the camera, a few globals), and hands the engine's call nothing to draw. The replay side does not
change: the same `Run` and `Draw` records, the same `Sky.hlsl`, clients, anchors, programs and Cloud Shadows handling.

Non-goals: the sun glare (outside the batch, stays the engine's), the sun occlusion query (step 4, after the batch), Physical
Sky's own passes.

## What the engine does (AE 1.6.1170; SE offsets in p5/sky.md)

**Which passes, in which order.** `BSSkyShaderProperty::GetRenderPasses` (AE `0x1414e6fd0`) gives each sky object one pass by its
type (property +0xC0):

| Type | Object | Pass id | Technique (raw) |
| --- | --- | --- | --- |
| 0 | sun | `0x5C000061` | Texture (7) |
| 1 | sun glare | `0x5C006072` | SunGlare (1), not in the batch |
| 2 | atmosphere | `0x5C00005E` | Sky (8) |
| 3 | cloud layer | `0x5C000062` when fBlendValue (+0xB8) <= 0; else `0x5C000064` (CloudsFade) when the base (+0x98) or the blend texture (+0xA0) is the null texture `DAT_14328ccb0` (bFadeSecondTexture +0xBE = base is null), else `0x5C000063` (CloudsLerp) | 4, 6, 5 |
| 4 | sky quad | `0x5C00005E` | Sky (8) |
| 5 | stars | `0x5C000060` | Stars (3) |
| 6 | moon | `0x5C000061` | Texture (7) |
| 7 | moon stars mask | `0x5C00005F` | MoonAndStarsMask (2) |

Cloud passes get accumulation hint `0x11` and go to geometry group 13's pass list (appended in registration order); every other
pass registers into its technique's batch group (pushed to the front). No pass for render modes 0xC-0xF, 0x10-0x11.

**The batch** (`RenderBatches`, AE `FUN_1414b4fe0`, called from FinishAccumulating +0x237 with `0x5C00005D..0x5C000064`): for each
technique in the range with registered passes (the batch renderer's active techniques, `FUN_1414f3710`), for each of its five
sub-groups in order, the sub-group's passes from the head (linked at pass +0x30), each through `RenderPassImmediately`
(`FUN_1414f3dc0`). Before a sub-group it sets: 0 back-face cull, no alpha test; 1 back cull, alpha test, alpha to coverage when
`DAT_14332a317`; 2 no cull, no alpha test; 3 no cull, alpha test, alpha to coverage when `DAT_14332a317`; 4 back cull, alpha test
(the cull only when the render flags hold neither 0x8 nor 0x100). After a sub-group, when the batch renderer's +0x6C (auto clear)
is set, it clears the sub-group's bit (group +0x28) and its head. FinishAccumulating sets the alpha reference to 128/255 before
the call and write mode 11 after it.

**The cloud layers** (`RenderPersistentPassList`, AE `0x1414f19d0`, group 13 as a pass list): each pass with geometry in list
order, the cull from the property's two-sided flag (bit 36 of +0x38), the alpha test from the geometry's effect data
(+0x120 -> +0x30 bit 9); then the list is emptied (+0x8, +0x10) and the batch renderer's pooled entries are returned.

**Per draw** (SetupTechnique, SetupGeometry; formulas in p5/sky.md): the technique's fixed state; `WorldViewProj`, `World`,
`PreviousWorld` from the geometry's world (+0x7C) and previous world (+0xB0) less the posAdjust, the view-projection at RSS+0x430;
`EyePosition` (accumulator +0x16C less the posAdjust); `VParams`; the cloud offset (`DAT_14338cb10[usCloudLayer]`); `BlendColor`
(property +0x88, or the sky's three colours for the atmosphere, or zero with w = 1 for the sky quad); `PParams`; t0-t2 from the
property or the defaults.

## Design

### Reading the passes

At the batch thunk (the existing one at +0x237), the family walks the sky technique groups of the accumulator's batch renderer
in the engine's order and takes each pass's geometry, property and pass id. The cloud thunk (+0x274) walks group 13's list.
This is reading what the engine registered, not intercepting its draws: no engine Setup* function runs and nothing is latched.

Then it hands the engine's call nothing to draw: for the batch, it detaches the sky sub-groups (bits and heads, as the auto clear
does), calls the engine's `RenderBatches` (which finds nothing and does its own bookkeeping), and puts them back only when +0x6C
is clear (the engine would have kept them). For the cloud list it empties +0x8/+0x10 and calls the engine's function, which then
draws nothing and returns the pooled entries itself.

### Computing the draws

Per pass, natively (`SkyNative.cpp`):

- **Technique** from the pass id (the table above), the pixel descriptor CS looks the shader up by (ShaderCache's raw index, plus
  DEFERRED in the main view as `ModifyShaderLookup` does).
- **State** from the technique (SetupTechnique, RestoreTechnique) and the sub-group (or the cloud pass's cull and alpha test), as
  engine mode indices, turned into the engine's own state objects (`Pipeline::EngineBlendState`, `EngineDepthStencilState`,
  `EngineRasterState`, read live since Deferred swaps entries) and their descriptions (`Family::DescribeStates`). The program key
  is cached per mode tuple and state object.
- **Constants** from the formulas above. The camera is read at the thunk (RSS+0x430, the accumulator's eye and posAdjust), so a
  face gets its face camera.
- **Textures** from the property (+0x98, +0xA0) or the defaults, through `Family::SlotTexture` (cached per frame).
- **Geometry** from the pass's geometry (`Family::LatchGeometry`, which reads the TriShape's buffers, not the draw).
- **Cloud Shadows**: `currentLayerForDraw` and the layer bookkeeping CloudShadows does in its SetupGeometry hook, called directly.

The run then goes to the existing `SkyDraws::Submit`.

### Parity (historical)

Until step 5, `CS_DCLF_SKY_NATIVE` kept the capture path (0) and an observe mode (1) that built the native run beside the capture
and compared the two draw by draw. Observe runs (`tools/cs-test/configs/sky-native.toml`: Riverwood at noon in cloudy weather with
the river's faces, a clear night, Dragonsreach) reported every draw equal: count, order, descriptor, states, every constant, textures,
samplers, geometry, viewport. Two capture artifacts were not differences: MoonAndStarsMask's PParams.y (its vanilla pixel shader
declares no PParams, so the capture reads zero; the native draw writes zero too), and IsAurora (bit 10 of the extra descriptor)
leaking into the captured face clouds from the sky-object effect drawn before them (Sky.hlsl does not read it). Measured at 960x540
(Dev-Fast): the engine draws through the family detour fell from 66 to 31 a frame and the family's zone from 0.63 to 0.29 ms, for
0.25 ms of native building (3 batches and 3 cloud groups); frame 6.41 -> 6.16 ms.

Found on the way and fixed: a cloud group whose head holds a sky-object effect (hint 0x12, BSEffectShader; at night) was left whole
to the engine, and Cloud Shadows' face layer draws on D3D11 are fatal in graph mode: native mode stopped with a message box every
night with reflection faces in view. The native path draws the clouds and leaves the effects at the group's head linked for the engine's call.

## Steps

1. Done: pass reading and draw computation, observe mode, parity log.
2. Done: native mode suppresses the engine's draws and submits the native run; screenshots match the capture path.
3. Done: cloud layers native (group 13), with Cloud Shadows' layer bookkeeping.
3a. Done: the build's cost cut from 4.6% to 2.0% of the render thread (960x540, Dev-Fast). Program keys and programs are cached
   by their inputs (the engine's state objects, descriptor, variant, targets); a texture slot's export lookup runs once a view a
   frame; the draws are built in place and the leases go straight to the run. Left: texture slots (0.5%) and geometry leases
   (0.3%), both already cached per frame.
4. Done 2026-11-25: the sun occlusion query without the engine's call. In mode 2 the hook on FUN_1414b4c00 (AE 106604, SE 99958)
   keeps the query's bookkeeping as the engine does (the accumulator's three entries at +0x70: the ring entry, created on first
   use, and the pending flag at +0x14; the disabled flag at +0x64), begins and ends the ring entry inside a Bracket, sets the
   query's camera, and builds the draw with `Native::BuildSun`: pass id 2 has no SetupTechnique case, so it is raw technique 0
   (SunOcclude), no alpha test, the sun's additive blend, and the eye from the shader manager's current accumulator (the screen
   splatter's after the world render, not the query's). Mode 1 compares it with the latched draw; observe runs (river noon and
   night, Dragonsreach) matched every field once SunOcclude's undeclared PParams.y was written as zero, as for MoonAndStarsMask.
5. Done 2026-11-25 (perf2-faces.md): the latch path, its switch and the parity helpers are deleted. What the latch path did that
   native did not: it left a draw out while its program built; native now does the same (`PassResult::LeftOut`). A batch or cloud
   group the native build cannot take (a pass that is not a BSSkyShader TriShape, a texture that cannot be imported) is drawn by the
   engine, logged once per reason. Rain, storms and the aurora were not covered by an observe run before the deletion.

## Risks

- A pass the batch holds that is not a BSSkyShader TriShape (another mod's sky object): left to the engine, as today.
- Mods that hook SetupGeometry for sky draws (Cloud Shadows is the known one, handled; others would no longer run).
- The engine's state tables changing between the read and the draw (Deferred's swaps happen outside frames).
