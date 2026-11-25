# ImageSpace without draw capture

Status: 2026-11-24, cheap cuts 1-2 done; steps 2a-2b (param, explicit and compute steps native) done; 2e done. 2026-11-25
(perf2-post): the switch `CS_ORG_IMAGESPACE_NATIVE` and its capture, observe and diagnostic modes are deleted; native staging is
the only path, and a step `NativeStepNow` declines (menus, image-based flares, geometry steps outside the chain) stays the capture's. Owner: the ImageSpace shim (`src/RenderGraph/Engine/ImageSpace/`). Background:
[p5/imagespace.md](../p5/imagespace.md) (the RE and the chain), [imagespace-remaining-ports.md](./imagespace-remaining-ports.md)
(the last effects), [dclf-native-sky.md](./dclf-native-sky.md) (the pattern this follows).

## Goal

CS draws each ImageSpace step from inputs it computes itself, and the engine's per-draw code draws nothing. Today the engine runs
every effect on the CPU, and each draw goes through `BSImagespaceShader::Render`, `RenderPassImmediately` (SetupTechnique,
SetupMaterial, SetupGeometry with a constant-buffer Map), and `DrawTriShape`. CS withholds the draw at `DrawTriShape`, forces
`SetDirtyStates`, copies the bound constant buffers into its ring (`Constants::Capture`), finds the step's pass, and stages it.

That is about 7.6% of the render thread in a Riverwood frame (960x540, Dev-Fast /Od, sampled). Instead, CS reads the engine
state that decides the effect values (ImageSpaceManager's blended data, INI settings, the camera, target sizes), writes each
step's constant rows itself, and opens the step's gate entry. The engine's container functions still run for their bookkeeping,
but their inner shader calls return at once.

The replay side does not change: the same step tables, passes, gates, latches and HLSL. Only the source of the constant rows and
the trigger change.

Non-goals: menu 3D scenes (they stay on capture, as DCLF's menu scenes do), draws with their own geometry or loaded textures
(water ripples, the underwater mask, lens flare sprites, ISAlphaBlend's tint masks) until the rest is done, and the effects the
remaining-ports plan still adds.

## What the engine does (AE 1.6.1170)

### How a step gets its constants

There are two ways, and the port needs both.

**Param effects** (most raster effects). The container fills an `ImageSpaceShaderParam` on the CPU, then calls the shader's
`Render(quad, param)` (107732, `0x1414f88b0`). The param's rows are set with `FUN_1414bed50(param, row, x, y, z, w)`, mostly in
the effect's `UpdateParams` (ImageSpaceEffect vtable slot 7), some by the container just before the call. The generic
`SetupGeometry` (`0x1414f8500`, shared by every ImageSpace shader checked) then copies `param+0x40` (pixelConstantGroup, as many
rows as the shader has PS constants) into PS b2 through the pixel shader's constant table, and writes VS row 0 as the input's
texel size plus the param's vertex rows. Every ImageSpace shader class shares the same SetupTechnique, SetupGeometry and
UpdateParams slots, so all the per-effect logic is in the containers.

**Explicit effects** (SAO, SSR, the compute set). The caller computes the values on its stack and calls a per-shader setter
(`BSImagespaceShaderISMinify::sub` `0x14151f710`, `ISMinifyContrast::sub` `0x14151f800`, `ISSAORawAO::sub` `0x14151f8f0`,
`ISSAORawAONoTemporal::sub` `0x14151f9f0`, and the like). The setter maps a custom constant buffer, copies the struct and binds it
at VS and PS b2 itself. Then the explicit `Render` (107733, `0x1414f8b00`) runs `RenderPassImmediately` with no param, and
SetupGeometry skips its copy (shader `+0x1a0` set).

PS b12 (PerFrame) is the engine's per-frame buffer. The ORG shaders read only its float4 members from it: the dynamic resolution
parameters and FrameParams (`ORG_SHARED_FRAMEBUFFER_AT`). The exception is ISSAOComposite, which also reads two matrices (see
"PerFrame at the SAO composite" below).

A param effect's "row" in `FUN_1414bed50(param, row, ...)` is a constant id, not a register. SetupGeometry maps each id through
the shader's constant table, and several ids can pack into one register. For example, the water displacement's ids 0-5 (Time,
BlendAmount, TextureOffset, fDamp, RainVars, WadingVars) land in c0.x, c0.y, c0.zw, c1.x, c1.yzw and c2. The native writer must
apply the same table. The explicit setters copy their struct straight into b2, so their struct order is the register order.

### Where the values come from (verified in Ghidra)

| Container | Function | Inputs |
| --- | --- | --- |
| HDR tonemap | `ImageSpaceEffectHDR::UpdateParams` `0x1414d13f0` | ImageSpaceData at manager+0xC8 (HDR +0xC8..0xE8, cinematic +0xEC..0xF4, tint +0xF8..0x104, fade +0x118..0x124), INI values (`LAB_142034568` and others), the UI menu state (`UI+0x160`), `powf` curves |
| HDR downsample, luminance, adaptation | `ImageSpaceEffectHDR::Render` `0x1414d0cb0` | rows 0-1 per draw: zero and 1/size of the input; the level loop halves sizes; parity `DAT_14338c960`; adapted value `LAB_142033da8` |
| Bloom blurs | `ImageSpaceEffectBlur::UpdateParams` `0x141516850` | radius from bloomBlurRadius (+0xCC), offset and weight tables `DAT_1435ef600` / `DAT_1435ef510` by radius and target size |
| VL effect (vanilla raymarch params, not the composite) | `ImageSpaceEffectVolumetricLighting::UpdateParams` `0x1414e3ad0` | the camera's near and far (id 8), the sun light's diffuse (id 15) |
| DoF, distant blur | `ImageSpaceEffectDepthOfField::UpdateParams` `0x1414e4d80` | ImageSpaceData DoF, effect members (+0x14C, +0x150), INI `DAT_14338ca90..aa8`, target sizes |
| Radial blur | `ImageSpaceEffectRadialBlur::UpdateParams` `0x1414f9a00` | ImageSpaceData +0x130..0x148, copied straight |
| Refraction | `ImageSpaceEffectRefraction::UpdateParams` `0x1414f9490` | manager refraction tint (+0x98), copied straight |
| SAO set | `RenderSSAO` 107245 `0x1414d5a50` | target sizes per level, the projection, SAO INI (`DAT_14338c980..9a`), through the explicit setters |
| SSR set | `RenderReflections` 107146 `0x1414ce240` | SSR INI, the world camera (`gWorldSceneGraph+0x128`), target sizes |
| TAA | TAA helper 107270 `0x1414d88b0`, rows in 107276 `0x1414d9080` | jitter, TAA block, sizes (below) |
| SAO composite | `RenderSSAO` second call | fog, camera, sun, snow sparkle INI, SSR INI (below) |
| Water blend | 107285 `0x1414da1d0` | camera near and far, menu flag, camera movement (below) |
| Upsample | 107283 `0x1414da010` | dynamic resolution ratios, input size (below) |
| VL / lens flare composites | 107213 `0x1414d45f0` | sun diffuse blended with the VL colour (below) |

Not read yet: FXAA and the world map. SE ids below are given only where the repo or CommonLib already names them; there is no SE/AE
id map here, so the rest are AE only.

Shared engine data the rows below read:

- **Current camera** `DAT_14332a328` (AE id 414580), an `NiCamera*`. World translate at +0xA0..0xA8, frustum at +0x150 (left,
  right, top, bottom, near +0x160, far +0x164). `Renderer::StartAccumulating` 1414a9190 and `Renderer::SubmitAccumulator` 1414a90f0
  set it, along with a few others. So it is whichever camera was submitted last, not always the world camera.
- **BSGraphics::State** at `0x14328cc20` (411479 / SE 524998): jitter `projectionPosScaleX/Y` +0x44/+0x48 (`0x14328cc64/68`),
  dynamic resolution ratios +0x104/+0x108, `dynamicResolutionLock` +0x118 (nonzero means the ratios count as 1).
- **Render target sizes**: RenderTargetManager `0x14328be80` (411451), 0x1C bytes per target, width at +0, height at +4.
- **Frame time** `DAT_142033084` (390954), written by `Main::Update`.
- **UI+0x160**: nonzero while a menu pauses the game. Several rows turn it into a 0/1 "menu" flag.
- **posAdjust** current `0x14202aecc` (388909) and previous `0x14202aed8` (388912), written by `SetCameraData`'s inner 77521.
  They are FrameBuffer's `CameraPosAdjust` and `CameraPreviousPosAdjust`.
- **Fog**: `BSShaderManager::State.shadowSceneNode[DAT_142033120]->fogProperty` (`FUN_14147ffd0`, 105637). Near colour +0x38, far
  colour +0x44, near +0x50, far +0x54, power +0x84, clamp +0x88.
- **Sun**: `shadowSceneNode[0]->sunShadowDirLight` (+0x210) `->light` (+0x48). Its world rotate column 0 (+0x7C, +0x88, +0x94)
  is the light direction, and its diffuse is at +0x11C. `shadowSceneNode[0]->sunLight` (+0x200) `->light` +0x11C is the sun's
  diffuse.

#### SAO composite (ISSAOCompositeSAO, SAOFog, Fog; ISCopy)

`RenderSSAO` (107245 / SE 100520) runs the composite at its end. The SAO owner is ImageSpaceManager+0x1C8. Its +0x50 is
`bSAOEnable`, cached at init (`FUN_1414d5930`). Fog is on when the fog property exists and `DAT_14338c99b` is set
(`bSAOApplyFog`, toggled by the `ToggleFog` console command). The form: with SAO, SAOFog (`ISSAOCompositeSAOFog::sub`, owner[5])
or SAO (108779 `0x14151f350`, owner[3]). Without SAO, Fog (108780 `0x14151f490`, owner[4]) or ISCopy (effect 0x22). Each is an
explicit setter that copies 11 rows (0xB0 bytes) in this order, which is the order `ISSAOComposite.hlsl` reads:

| Row | Name | Value |
| --- | --- | --- |
| 0 | FogParam | x = near / (far - near), y = 1 / (far - near), z = power, w = clamp. If far == near: (5e6, 0.1, 1, 0) |
| 1 | FogNearColor | fog near colour, w = `DAT_1420330fc` (390973, the framebuffer range `SetFramebufferRange` writes) |
| 2 | FogFarColor | fog far colour, w = the same |
| 3 | CameraNearFar | current camera near, far, 0, 0 |
| 4 | LightDirection | sun direction normalized (0 if its length is under 1e-6), w unset |
| 5 | EyePosition | current camera world translate, w = 1 if `bEnableImprovedSnow` else 0 |
| 6 | SparklesParameters1 | `fSparklesIntensity`, `fSpecularSparklesIntensity`, `fSparklesSpecularPower`, `fSparklesDensity` (minus 0.5 with `bUsePrecomputedNoise`) |
| 7 | SparklesParameters2 | `fSparklesSize` (plus 1 with `bUsePrecomputedNoise`), `fNonSpecularSparklesIntensity`, `bToggleSparkles`, `bSparklesOnly` (as 0/1) |
| 8 | SparklesParameters3 | `bDeactivateAOOnSnow` (0/1), `fSparklesMaxDistance`, `bUsePrecomputedNoise`, 0 |
| 9 | SparklesParameters4 | `fShadowSparkleIntensity`, then the colour by `iSnowSparklesColor`: 0 owner+0x58, 1 owner+0x64, 2 sun diffuse, 3 `fSnowSparklesColorR/G/B`, else 0 |
| 10 | SSRParams | `fReflectionsIntensityScale` x 0.8, `fClampScale`, SSR on (`DAT_14338c99d`, 0/1), w unset |

All INI names are `:Display`. `DAT_14338c99d` is set by `Main::RenderWorld` from ImageSpaceManager+0x1C0 +0x10 just before the
second `RenderSSAO`. In the SAO-only form rows 0-3 are stale stack values; the shader reads them only under `APPLY_FOG`. The owner's
+0x58/+0x64 colours are zeroed at init and no writer was found, so modes 0 and 1 likely give black. Everything here is fixed for
the frame by the time `RenderSSAO` runs.

#### PerFrame at the SAO composite

The IBL and height fog paths (`ISSAOComposite.hlsl:266`, `:287`) and snow sparkles (`:319-321`) read
`FrameBuffer::CameraViewProjInverse` and `CameraPosAdjust`.

- **Which buffer.** The ORG build defines `ORG_SHARED_FRAMEBUFFER_AT PerGeometryAt` (`ISSAOComposite.hlsl:10`), so FrameBuffer is
  the raw load from the latch's `FrameSrv` / `FrameOffset` (`Common/NamespacedCBuffer.hlsli:48-67`), not CS's shared snapshot
  (`SharedConstants`, a real cbuffer copied from `frameBufferCached`). `StageStep` fills those from the capture's PS b12 slot
  (`WorldSteps.inl:1119-1129`), and it is fatal if no b12 is bound. So it is the engine buffer bound at that draw.
- **Is it valid.** Yes. The engine has one PerFrame buffer (524768 / 411384, `0x143288788`), mapped only by 77258 (`0x140e45680`).
  That runs from `State::SetCameraData` (75694 / 77503, via 77521) and from the accumulator's batch render (99939 / 106584).
  `Main::RenderWorld` (1414cdaa0) calls `Renderer::StartAccumulating(world camera)`, which calls `SetCameraData`, then both
  `RenderSSAO` calls, with nothing in between that sets a camera. So at the composite b12 holds the world camera's jittered
  matrices. These are the same bytes `frameBufferCached` holds (the last unmap) and the same bytes the D3D11 build reads.
- **What was wrong** was the load, not the data: see the Risks entry (fixed in 7822cbf5c). After `RenderWorld`,
  `SubmitAccumulator(screen splatter camera)` rewrites b12 and the current camera. Later steps see that camera, not the world's.

#### TAA (ISTemporalAA, ISTemporalAA_Water, ISTemporalAA_UI)

`RenderEndOfFrameEffects` (105674 / SE 99023) calls the helper 107270 with the TAA object at ImageSpaceManager+0x1F0. The object:
+0x18 on, +0x2C mode (2 is the UI form), +0x30 a reset count (`DAT_1420353a8`, set by `ConfigureTAA`, forces 0x3F), +0x34
history parity, +0x38 water blending. The parity flips on every call except the UI form, before the draw. The rows come from
107276 `0x1414d9080` (the water form's builder `0x1414d9a80` is identical) and `ISTemporalAA::sub` (`0x14151fe70`) copies 6 rows.
The order matches `ISTemporalAA.hlsl:39-44`:

| Row | Name | Value (W, H = the output target's size; jx, jy = State jitter) |
| --- | --- | --- |
| 0 | TexelSizeParams | 1/W, 1/H, trunc(W x rw)/W, trunc(H x rh)/H, where rw, rh are the dynamic resolution ratios (1 when locked) |
| 1 | JitterAndRes | 0.5 jx, 0.5 jy, 0, 0 |
| 2 | NeighborWeights | four bilinear weights, normalized to sum 1, of a tent t(v) = 1 - 2\|v\| (0 outside [-0.5, 0.5]) at k/2 x (0.5 jx W) and k/2 x (0.5 jx W + sx) on x, and likewise on y. sx = -1 if jx > 0 else +1, same for sy. k = `DAT_1420353ac` (1.0) |
| 3 | TexelOffset | sx/W, sy/H, 0, 0 |
| 4 | BlendParams | `DAT_1420353b0..bc` (0.5, 0.8, 0.21, 0.21), changed only by `ConfigureTAA` |
| 5 | ThresholdParams | current camera near, far, the menu flag, `fTAAEffectThreshold` |

It also stores row 0 at the object's +0x1C..+0x28. The jitter is set once per frame, before the world camera. Row 5 reads the
current camera at end of frame, which can be the screen splatter's camera (see above).

#### Water blend (ISWaterBlend)

107285 `0x1414da1d0` is called from the accumulator's batch render (106584), so it runs inside the water pass, while the world
camera is current. The object's +0x08 is on, +0x0C asks to clear the three history targets. History parity is
`gFrameCounter & 1` (RT1 = 0x59 + parity, input 0x5A - parity). `ISWaterBlend::sub` (`0x141520240`) copies one row:

- Row 0 (`NearFar_Menu_DistanceFactor`): current camera near, far, the menu flag,
  max(0, 1 - 0.025 x length(posAdjust - previous posAdjust)).

#### Upsample (ISUpsampleDynamicResolution)

107283 `0x1414da010`, last in `RenderEndOfFrameEffects`. It draws only when the lock is 0 and a ratio is not 1. One row
(`FUN_141520170`):

- Row 0 (`g_UpsampleParameters`): rw, rh, (trunc(W x rw) - 1)/W, (trunc(H x rh) - 1)/H, where W, H are the input target's size.

#### VL and lens flare composites (ISCompositeVolumetricLighting, ISCompositeLensFlare, ISCompositeLensFlareVolumetricLighting)

`RenderPreTonemapComposites` (105675 / SE 99024) runs SIL (`0x1414d79a0`, CS ignores its rows), IBLF (`0x1414d21b0`, forced off
by CS), then 107213 `0x1414d45f0`. The form depends on `gVolumetricLightingEnabled` with ImageSpaceManager+0x170 +8 (VL), and
ImageSpaceManager+0x180 +0x30 (IBLF lens flare). One row:

- Row 0 (`VolumetricLightingColor`): `FUN_1414c49a0` (107017): sun shadow light diffuse + (VL colour - that diffuse) x
  `fVolumetricLightingCustomColorContribution`, w = 1.

The VL settings block is a static from `FUN_1414c4820` (107014 / SE 100297): +0 `fVolumetricLightingIntensity`, +4 the custom
colour contribution, +8..+0x10 the custom colour. No direct writer of +8..+0x10 was found (likely a block copy from the weather's
VL form), so read it through 107014 at the thunk.

ISApplyVolumetricLighting (107193 / SE 100475, `0x1414d1af0`), if it is ever drawn instead of CS's compute: row 0 =
(intensity / settings+0x38, `fVolumetricLightingTemporalAccumulationFactor` if
`bVolumetricLightingEnableTemporalAccumulation` and no history reset (object+9) else 0, 0, 0).

#### HDR chain, per level

Adds to the two HDR rows in the table above. Ids are constant ids (see the note at the top).

- **DownSample4**: id 0 = (1, 0, 0, 0), id 1 = (1/w, 1/h) of the input.
- **RGB2Lum, then 16Lum per level**: id 0 = 0, id 1 = 1/size of that level's input. Each level divides sizes by 4, rounding up,
  down to 1 (2 when `LAB_142034550` is set and `DAT_14338c95d` is clear). On the last level the parity `DAT_14338c960` (415054 / SE
  528109) flips and picks the target (0x1B + 2 x parity).
- **16LightAdapt** (only when the adapted target exists, `DAT_142033db0 >= 0`): inputs are the last level and the adapted value
  `LAB_142033da8` (391302 / SE 513464). id 3 = 1/size of the last level. The adaptation ids come from HDR UpdateParams: id 2 =
  (0, 0, 1 - pow(max(0, 1 - 0.01 s), 30 dt), 1 - pow(max(0, 1 - (0.01 / (`fGlobalEyeAdaptStrengthScale` x data+0xE8)) s), 30 dt)),
  where s = `fGlobalEyeAdaptSpeedScale` x data+0xC8 and dt = the frame time, or `fConstHDRAdaptTimerForMenu` while the menu flag is
  set. The output becomes the new adapted value.
- **Bright pass blur**: the blur effect at manager+0x28 slot 0x17 + clamp(bloomBlurRadius - 1, 0, 6). Its threshold row is
  `fGlobalBloomThresholdBoost` (`fGlobalMapBloomThresholdBoost` on the map) + data+0xD0, then data+0xD4. Input 3 is the adapted
  value.
- **Tonemap**: HDRTonemapBlendCinematic, or its Fade form when data+0x118 > 0 (then id 4 = the fade colour and amount). Ids 1-3 as in
  the table (`bUseFilmicCurve`, `fReinhardWhiteScale` / `fFilmicWhiteScale`, the `fGlobal*Boost` values).

#### Start stage water: displacement and flow copies

- **Displacement** (`ImageSpaceEffectWaterDisplacement::Render`, 107582 `0x1414ef370`). This is a simulation. Effect+0x98 adds the
  frame time each frame, and the sim steps at a fixed 0.025 s (+0x9C, +0xA8), so the number of step draws per frame varies (0 to
  several). Ids: 0 Time = +0x98, 1 BlendAmount = `0x1433dcef8` (415231, 0 from init), 2 TextureOffset = `0x1433dcf20/24`
  (written by the water system, `FUN_140522b60`), 4 RainVars = `0x1433dcf78..84` (init constants), 5 WadingVars =
  `0x1433dcf88..94` (set by 32176 / SE 31385 when wading). Wading ripples are geometry: one quad per queued ripple
  (`0x1433dcf28` / `0x1433dcf40`, count `0x1433dcf38`) with vertex rows from `FUN_1414becc0`. The height map pair swaps after each
  draw.
- **Flow copies** (ISCopySubRegionCS). `DispatchCopySubRegion` (107217 `0x1414d48c0`) copies the object's +0x40 block (2 rows)
  with `ISCopySubRegionCS::sub` (108770). `FUN_1414d4a80` (107221) sets it: row 0 = (src x, src y, width, height), row 1 = (dst x,
  dst y, 0, 0). 36560 / SE 35561 drains BGSAutoWater's request ring (+0x1008, read +0x1808, write +0x1810). Per request: row 0 =
  (rec+0x14, rec+0x08, rec+0x0C, rec+0x10), row 1 = (rec+0x18, rec+0x1C). The source is the request's loaded texture, the destination
  `DAT_14332a460+0x48`. The count per frame depends on loading. The snow window copy in `RenderWorld` uses (0, 0, main W, main H,
  0, 0).

#### Lens flares (ISLensFlareVisibility, ISLensFlare sprites)

106988 `0x1414c2600` (when `LAB_142033658` is set) gathers up to 16 lights in 106989 `0x1414c2a90` into `DAT_14338c2b0` (16 bytes
per light: screen position and related values from the camera projection, with frame-to-frame history in `DAT_14338c230..260`).
It then copies all 16 rows into the visibility param (`FUN_1414bed90`, 0x100 bytes). The sprites draw per-flare geometry from
BGSLensFlare records. Both depend on per-frame light gathering and geometry, so they stay out of scope (Non-goals).

### The steps of a frame

Hooks are the ones in `ImageSpace.cpp`. "Rows" are the PerGeometry rows CS's ORG shader reads (`OrgEngineRow`). Every row
below also reads the PerFrame float4s, except the compute ones.

**Exterior world frame.** About 40-60 steps, depending on SAO levels, luminance levels, water and lens flares (step 0 counts
them).

| Stage, scope (hook) | Steps | Rows | Targets | CS ignores the engine's rows? |
| --- | --- | --- | --- | --- |
| Start, water update (31385 / 32176) | Displacement: TexOffset, Wading/Rain heightmap, Blend, Smooth, Normals; ripples (geometry) | 0-2 | `kWATER_DISPLACEMENT*` and the simulation targets | No |
| Start, flow copies (35561 / 36560) | ISCopySubRegionCS, up to 32 | 0-1, plus the source by heap index | the flow map | No |
| Deferred, reflections (100428 / 107146) | ISDownsampleIgnoreBrightest (or its CS form), ISReflectionsRayTracing, ISReflectionBlurH/VCS | 0-16, 0-1, 0-1 | SSR mask, `kSSR_RAW`, `kSSR_BLURRED0`, `kSSR` | No |
| Deferred, RenderSSAO first call (100520 / 107245) | ISSAOCameraZ, ISMinify per level, one ISMinifyContrast, ISSAORawAO or NoTemporal, ISSAOBlurH, ISSAOBlurV | 0-1, 0-2, 0-3, 0 | `kSAO_CAMERAZ` mips, CS's SAO history and temp, `kSAO` | No |
| Deferred, snow window | ISCopySubRegionCS kMAIN into `kSNOW_SWAP` (ISSnowSSS with snow) | 0-1 (0-1) | `kSNOW_SWAP` | No |
| Deferred, RenderSSAO second call | ISSAOCompositeSAO, SAOFog or Fog (ISCopy with neither) | 0-10 | kMAIN | No (the plain ISCopy form: yes) |
| Deferred, water blend (own scope) | ISWaterBlend | 0 | kMAIN, water history pair | No |
| Post, lens flares (SE 100274) | ISLensFlareVisibility, ISLensFlare sprites | light rows; sprite records | `kLENSFLAREVIS`, kMAIN | No |
| Post, composites (99024 / 105675) | ISCompositeVolumetricLighting (or LensFlare / both), ISSILComposite | 0; none | kMAIN | VL: no. SIL: yes |
| Post, HDR effect (100465 / 107183) | ISHDRDownSample4, RGB2Lum, 16Lum per level, 16LightAdapt, BrightPassBlur H, Blur V, tonemap | 0-22 (the blur offsets), 0-5 for the tonemap | HDR downsample chain, CS's adapt pair, bloom pair, the final target | Tonemap under Post Processing: yes (BLEND passthrough). Effects11 skips the whole effect |
| Post, end of frame | ISTemporalAA (not drawn when Upscaling replaces TAA), ISUpsampleDynamicResolution (dynamic resolution only) | 0-5, 0 | final target, CS's TAA history | No |

**Only with an ImageSpace modifier or a setting** (not in a plain frame): refraction (row 0), DoF and distant blur (0-7),
radial blur (0-1), full-screen blur and get hit (ISBlur, ISCopy, ISDoubleVision), FXAA, the world map, the LDR slots, IBLF
(forced off by CS), the compute SAO, the screenshot and frozen-capture copies.

**Interiors.** The same chain. The scopes pick other forms (the SAO composite's fog form, SSR as set), and water and lens flares
are usually absent.

**Menus and loading screens** (`MenuSteps.inl`): the menu 3D scene's HDR chain and tonemap, ISCopy into the framebuffer, the
frozen composite, ISSimpleColor and ISTemporalAA_UI, and the face tint (ISAlphaBlend). These stay on capture.

**Already independent of the engine's PerGeometry**: plain ISCopy (it reads only the PerFrame dynamic resolution values),
ISSILComposite (the same), the tonemap under Post Processing, and ISMap and ISNoise (taken with no pass). Everything else reads
captured rows.

## Where the 7.6% goes

From the render-thread samples behind the cost data (`s6`, 330 of 4366 samples have an ImageSpace frame). Shares are of the
whole render thread. Dev-Fast is /Od, which inflates CS's std::map, fmt and lambda code more than the engine's code, so Release
shares will differ.

| Part | Share | What |
| --- | --- | --- |
| Under the engine's inner calls | 2.6% | `RenderPassImmediately`: CS's BeginTechnique hook 1.0% (of it `ResolveImageSpaceDescriptor` 0.8%, a string hash or a linear scan of names per draw, twice), CS's SetDirtyStates and Map hooks, and the engine's own code (0.4% leaf in SkyrimSE) |
| Shim bookkeeping | 3.9% | `StageStep` 1.6% (the forced `SetDirtyStates` 0.7%, `Constants::Capture` 0.5%), `StepGroup::Find` and `Held` 0.9% (`Frame::Detail::HeldOf` 0.7%), `fmt::format` of the step names on every draw 0.3%, `CommitBatch` 0.2%, the rest small |
| Batch runs | 1.0% | `RunAnchor` into `FrameAnchors::Run`, which also runs pending DCLF epochs (0.6%) that would otherwise run at the next tap |

A native port removes the first row and most of the second: no `RenderPassImmediately`, no capture, no forced state apply, no
per-draw find. What stays: the anchor runs (about 1.0%, less if a scope's batches merge), the container functions' own code
(about 0.4%), and the native constant building (estimated 0.2-0.3% in Dev-Fast, since the inputs are a few dozen floats a step).
So about 7.6% down to 1.5-2%.

### Cheap cuts without a port

These help today and stay useful after the port. Together about 2%.

1. **Cache the ImageSpace descriptor per shader** (`SShaderCache::ResolveImageSpaceDescriptor`, `ShaderCache.cpp`). The name
   lookup hashes a C string or walks `CheckedImagespaceShaders()` with string compares, twice per draw. A small map from the
   `BSImagespaceShader*` to the descriptor makes it a pointer lookup. 0.8%.
2. **Build the step names only on the error paths.** `TakeWorldStep`, `TakeChainStep`, `TakeSaoCsStep` and `TakeLensFlareStep`
   format `what` before they know whether they need it. 0.3%.
3. **Hold the frame's builds once per tap.** `StepGroup::Find` walks the kept builds' `std::map` and then asks `Held` for the
   placement on every draw. Cache the held build per part at Begin and at each stage change, and key the entries by a flat hash of
   (scope, effect, variant). Most of 0.9%.
4. **Capture without the full state apply.** `StageStep` calls `SetDirtyStates` so the bound constant buffers reach the capture.
   Reading the shadow state's pending PS b2 and b12 buffers would skip the targets, blend, samplers and the other binds. Up to
   0.7%. Riskier than 1-3: the capture's tracker must see the same buffer version.
5. **Merge batch runs within a scope.** RenderSSAO's two calls and the snow window run three anchor runs where one would do.

## Design

### Trigger and suppression

Follow the sky: keep calling the engine's container functions, so they do their own bookkeeping (luminance parity
`DAT_14338c960`, the adapted value `LAB_142033da8`, TAA and water history, SAO temporal state, render target bookkeeping), and
let the inner shader calls draw nothing. In native mode the `ShaderRender`, `ExplicitRender` and `DispatchComputeShader` thunks
return at once for a step CS covers: no `RenderPassImmediately`, no `DrawTriShape`, no capture.

The step list is already deterministic. The frame's description at Begin picks the rows (`WorldScopeRows`, `ChainRows`) from the
engine state that selects them (`WorldFields`, `PostFields`). What the hooks still decide late is the variant (normals parity,
history parity, the luminance level, the IBLF repeat). Native mode computes those at the container thunk from the same globals
the hooks read now (`EngineNormalsParity`, `EngineFrameCounter`, `LuminanceParity`).

At each container thunk (`HDRRender`, `RenderSSAO`, `RenderReflections`, `PreTonemapComposites`, the end-of-frame hook, the water
hooks), before calling the engine's function, CS:

1. lists the container's on steps for this frame from the held build (one `Find` per container, not per draw),
2. computes each step's constant rows and PerFrame values natively,
3. writes them into a CS-owned region of the constants ring and stages a `Pending` per step,
4. calls the engine's function, whose inner draws now return at once,
5. runs the batch once at the scope's end, as today.

The step's targets and inputs come from the step table, as now. Where the hooks read bound state today (`IblfVariantNow` reads
the shadow state's t0 and RT0; `CheckStepState` compares the shadow state), native mode reads the effect's texture list
(ImageSpaceEffect +0x48: output first, then the inputs) instead.

### Computing the constants

A function per container family writes the rows in the ORG shader's row order (the vanilla packoffsets that `OrgEngineRow` reads):

- **HDR**: rows from the HDR UpdateParams and Render formulas above. Input sizes from the step table. The frame time and adaptation
  values are read at the HDR thunk, where the engine reads them.
- **Bloom blurs**: the offset and weight tables by radius and size. They are fixed per size, so cache them per build.
- **SAO**: RenderSSAO's per-level sizes and the projection, the SAO INI. Same for the compute SAO when it is on.
- **SSR**: RenderReflections' INI values and the camera.
- **Composites, slots, TAA, upsample, water blend**: one function each, from the table above, after their decompile.

PerFrame: write the dynamic resolution parameters and FrameParams from CS's own values (`graphicsState` ratios, the clamp offset,
the gamma), the same ones CS's shared FrameBuffer snapshot holds.

### An interim: read the engine's values

Before every family has its formulas, a step can read the values the engine already computed, without the D3D path:

- **Param effects**: at the `ShaderRender` thunk, read `param+0x40` (and its row count) and map it through the pixel shader's
  constant table, as the generic SetupGeometry does. Then return without calling `Render`.
- **Explicit effects**: hook the per-shader setters (`ISMinify::sub` and the rest) and keep the struct they are given. The
  explicit `Render` then returns at once.

This is still the engine's per-effect CPU code, so it is a stepping stone, not the goal. It removes the same per-draw D3D work as
the full port (`RenderPassImmediately`, the capture, the state apply), so it is worth landing first, and it is the reference the
observe mode compares the native rows against.

## Switch and parity (historical: the switch was deleted on 2026-11-25)

`CS_ORG_IMAGESPACE_NATIVE`, per family where it helps (HDR, SAO, SSR, composites, chain, water):

- 0: capture, as today.
- 1 (observe): the capture draws. The native path computes every step's rows before the container runs. After each draw's capture
  the two are compared row by row (each float within 1e-5 relative), plus the PerFrame float4s, the step key, the variant, and the
  targets from the effect's texture list against the bound ones. Mismatches log once per effect and row with both values.
- 2 (native): the native rows draw and the inner shader calls return at once. The capture hooks stay installed to answer the
  thunks of effects not covered yet.

Observe runs: Riverwood at noon and at night (SSR, SAO, water, lens flares, HDR adaptation), Dragonsreach (interior forms, fog
composite), a weather change and an ImageSpace modifier (a potion, a hit: DoF, radial blur, get hit, full-screen blur), dynamic
resolution on, Upscaling on and off (TAA), Post Processing on and off (tonemap). Expect capture artifacts as with the sky: rows a
vanilla shader does not declare read as zero in the capture.

## Steps

0. Count a frame's steps per scope in the cs-test scenes (log the `taken` counters per scope once a second). Gives the per-family
   weights for the order below.
1. The cheap cuts 1-3 (descriptor cache, lazy names, held-build cache). No behaviour change. Measure.
2. Native trigger and suppression with the interim source: param reads and setter hooks, effect texture lists instead of the
   shadow state, observe mode comparing them with the capture. Then mode 2 for all non-geometry steps. This takes the engine's
   per-draw path and the capture off the render thread.
   2a. Done 2026-11-24: param steps (`ShaderRender`, base Render AE `0x1414f88b0`) of the world, start and post chain scopes,
   modifier slots included. `NativeSteps::BuildRows` copies the param's pixel constants (+0x40, one float4 per id the shader
   lists at +0x160, none when +0x1A0 is set) to the vanilla pixel shader's table offsets, as the generic SetupGeometry does;
   PerFrame is CS's `frameBufferCached`, uploaded when it changes (`Constants::Upload`). Observe mode compared 41,733 draws with
   the capture: no row and no PerFrame word differed. Draw mode keeps what base Render leaves for later code: `PreRender` and
   `PostRender` run around the staging, and the shadow state's render targets are set as its target bookkeeping (77456) would set
   them (`KeepRenderTargets`). Without that, the Scaleform displays read a stale render target and refused (a fatal tap error),
   and after a native distant blur the engine drew an extra ISCopy over the frame. Still captured: lens flares, image-based
   flares (their variant reads the bound t0 and target), geometry and loaded-texture steps (ripples, ISAlphaBlend), menu steps,
   explicit (SAO, SSR) and compute steps. Measured at 960x540 (Dev-Fast): ImageSpace 7.0% -> 6.5% of the render thread;
   `ShaderRender` 1.0% -> 0.6%, the shim's `DrawTriShape` 2.6% -> 1.6%. `CS_ORG_IMAGESPACE_NATIVE=3` (a diagnostic) stages the
   computed rows at the engine's own draw.
   2b. Done 2026-11-24: explicit and compute steps. An explicit step's setter (ISMinify's at AE `0x14151f710` and the rest) maps an
   engine pool buffer, copies its struct and binds it at PS b2 on the context before the explicit Render (AE `0x1414f8b00`:
   PreRender, the draw, PostRender, no target), so `NativeSteps::BoundRows` captures PS b2 at the hook with no state applied; observe
   mode compared 118,162 draws (param and explicit) without a difference. A dispatch's CS b0 rows are written by the effect before
   `DispatchComputeShader` (AE `0x1414f8be0`), and `Renderer::DispatchCSShader` (AE `0x140e49cb0`) only applies state, binds that
   buffer and dispatches, so a native dispatch binds it, captures it and skips both. Still captured: the compute SAO (its first
   dispatch checks the bound views) and the flow copies (their source is the bound view), with the steps listed in 2a. Measured at
   960x540 (Dev-Fast): ImageSpace 7.0% (before 2a) -> 5.5% of the render thread; the shim's `DrawTriShape` no longer shows.
   2c. Done 2026-11-24: the image-based flare draws take their variant's targets from the effect's texture list (output texture 0,
   input slot i texture i + 1, as base Render and the generic SetupGeometry bind them); observe mode cross-checks the list with
   the bound targets. Not yet exercised: `iblf.toml` did not run the flares in capture mode either. The flow copies and the
   compute SAO go native as they are: their callers set their views in the shadow state before the call. Left captured: lens
   flares, ripples, ISAlphaBlend, menu steps and ISDownsampleCS. The explicit steps still read the PS b2 their setter bound.
   2d. Done 2026-11-24: lens flares. The visibility pass is a param step. A sprite's draw is made whole (`NativeSteps::BuildSprite`):
   VS b2 row table[0] = (0, 0, 0.5 / width, 0.5 / height) of the output at the dynamic-resolution ratio (`DAT_141ad285c` is 0.5;
   the ratio is 1 while `dynamicResolutionLock` is set), rows 1 to n - 1 (n at shader +0x148) from the param's vertex constants;
   the quad's whole index range; slot 0 from the effect's texture 1. Both constant groups count float4 rows
   (`pixelConstantGroupSize`, `vertexConstantGroupSize`), and the param must be an `ImageSpaceShaderParam` (vtable checked; some
   Render calls pass another param). Observe mode matched every sprite field but one: the capture's `GlobalIntensity` (PS c2.y,
   constant 3) is 0, because CS's compiled D3D11 shader leaves it out of its constant table so the engine never writes it, and the
   ORG pass multiplies the tint by it: captured sprites drew black. The native rows use vanilla's table, so the sprites get the
   engine's intensity (1 by default). `RE::BSGraphics::RenderTargetManager::GetSingleton()` reads the manager's address as a
   pointer; the manager is the object at 524970 / 411451.
   2e. Done 2026-11-24: wading ripples (their quad from the geometry passed to Render) and the menu steps: a UI scene step's target
   and t0, and the face tint's sixteen masks (ISAlphaBlend), come from the effect's texture list (texture 0 the output, texture
   i + 1 slot i: an engine target, a depth index, or a loaded texture's view at +0x48 -> +0x10), cross-checked in observe mode
   (the inventory, the map, a menu 3D scene with TAA, loading screens: no difference). Every ImageSpace step now draws natively
   except ISDownsampleCS (the image-based flares' dispatch) and the no-raster steps. The capture path stays for modes 0 and 1:
   observe mode is how each later change is checked.
3. HDR native (the most draws in every frame, formulas read): downsample, luminance, adaptation, bloom, tonemap.
4. SAO native (the next most draws), then SSR.
5. Composites (VL, SIL), the SAO composite, the water blend, TAA and the upsample: one decompile each.
6. The start stage's water: displacement and flow copies, from the water system (ties into the water family's port).
7. The modifier slots (refraction, DoF, radial blur, blurs, FXAA, world map): rare but simple, mostly ImageSpaceData copies.
8. Geometry and loaded-texture steps (ripples, underwater mask, lens flares, tint masks) if still worth it. Menus stay on capture.
9. Delete the per-draw capture path for the families observe runs have covered.

## Risks

- **Engine bookkeeping inside the inner calls.** `BSImagespaceShader::Render` also binds targets through the RenderTargetManager,
  sets the manager rectangle's viewport and the shadow-state flags. Skipping it leaves the shadow state different for later engine
  code and for CS hooks that read it. Find each reader before mode 2 (the IBLF variant and `CheckStepState` are known).
- **CS's own hooks on the inner path.** `WaterBlendHistory::BSImagespaceShader_Render` clears RT1 before the water blend. The
  tonemap call-site hook (`Main_HDRTonemapBlendCinematic_Render`) runs the Tonemap anchor, Effects11 and Post Processing. The
  `CSShadersSupport` dispatch hooks pick CS's compute replacements (volumetric lighting). Each must still run, or move to the
  container thunk.
- **Other mods** that hook an effect's UpdateParams or the shader Render (Legacy Graphics Compatibility patches the rectangle's
  viewport call). With mode 2 their changes to the param no longer reach the draw unless the native path reads the same inputs.
- **Timing of the inputs.** Values like the frame time, the adapted luminance and the fade must be read where the engine reads
  them, or a frame's HDR differs by one frame. Observe mode catches this.
- **The SAO composite's FrameBuffer matrices** (checked 2026-11-24, fixed). Steps load the captured PerFrame raw
  (`ORG_SHARED_FRAMEBUFFER_AT`). DXIL keeps `row_major` in a templated raw load, DXC's SPIR-V does not, so on native Vulkan
  `CameraViewProjInverse` came back transposed. It reached the screen in the composite's fog (only before View Atmosphere's
  post-water composite switches in, after which `PostWaterComposite` skips it) and in snow sparkles. The raw path now transposes
  the matrices back on SPIR-V, and menu scenes, which pre-transposed their latched copy for SPIR-V (and so were wrong on D3D12),
  latch it as bound. Native constants must write PerFrame as the cbuffer holds it.
- **The current camera changes between scopes.** `DAT_14332a328` and the PerFrame buffer belong to whichever camera was submitted
  last. The SAO composite and the water blend see the world camera. TAA's ThresholdParams (near, far) at end of frame can see the
  screen splatter camera that `RenderWorld` submits after the world. Native rows must read the global at the container thunk, not
  CS's world camera, or observe mode will flag near and far.
- **Constant ids are not registers.** Param effects pack ids through the shader's constant table (the water displacement packs 6 ids
  into 3 registers). Writing ids as rows reads garbage. Take the table from the shader at build time.
- **Variable step counts.** The water displacement sim draws 0 to N fixed 0.025 s steps per frame, and the flow copies drain a load
  queue. Their step lists cannot come from the frame's description at Begin. They need a count at the thunk, or they stay on the
  interim source.
- **Late-built shapes.** A draw the build has no pass for is a tap error today. In native mode the step list comes from the build,
  so an engine draw with no step is invisible unless the inner thunk still checks coverage and logs it.
