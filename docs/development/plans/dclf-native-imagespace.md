# ImageSpace without draw capture

Status: plan, 2026-11-24. Nothing built. Owner: the ImageSpace shim (`src/RenderGraph/Engine/ImageSpace/`). Background:
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
parameters and FrameParams (`ORG_SHARED_FRAMEBUFFER_AT`).

### Where the values come from (verified in Ghidra)

| Container | Function | Inputs |
| --- | --- | --- |
| HDR tonemap | `ImageSpaceEffectHDR::UpdateParams` `0x1414d13f0` | ImageSpaceData at manager+0xC8 (HDR +0xC8..0xE8, cinematic +0xEC..0xF4, tint +0xF8..0x104, fade +0x118..0x124), INI values (`LAB_142034568` and others), the UI menu state (`UI+0x160`), `powf` curves |
| HDR downsample, luminance, adaptation | `ImageSpaceEffectHDR::Render` `0x1414d0cb0` | rows 0-1 per draw: zero and 1/size of the input; the level loop halves sizes; parity `DAT_14338c960`; adapted value `LAB_142033da8` |
| Bloom blurs | `ImageSpaceEffectBlur::UpdateParams` `0x141516850` | radius from bloomBlurRadius (+0xCC), offset and weight tables `DAT_1435ef600` / `DAT_1435ef510` by radius and target size |
| VL composite | `ImageSpaceEffectVolumetricLighting::UpdateParams` `0x1414e3ad0` | the VL colour and the camera's near and far |
| DoF, distant blur | `ImageSpaceEffectDepthOfField::UpdateParams` `0x1414e4d80` | ImageSpaceData DoF, effect members (+0x14C, +0x150), INI `DAT_14338ca90..aa8`, target sizes |
| Radial blur | `ImageSpaceEffectRadialBlur::UpdateParams` `0x1414f9a00` | ImageSpaceData +0x130..0x148, copied straight |
| Refraction | `ImageSpaceEffectRefraction::UpdateParams` `0x1414f9490` | manager refraction tint (+0x98), copied straight |
| SAO set | `RenderSSAO` 107245 `0x1414d5a50` | target sizes per level, the projection, SAO INI (`DAT_14338c980..9a`), through the explicit setters |
| SSR set | `RenderReflections` 107146 `0x1414ce240` | SSR INI, the world camera (`gWorldSceneGraph+0x128`), target sizes |
| TAA | TAA helper `0x1414d88b0` | jitter, TAA INI, sizes |

Not yet read: the SAO composite's rows (fog from the sky, camera near and far, sun direction, eye, snow sparkles, SSR params),
the water blend, the upsample, the water displacement (`0x1414ef370`), the flow copies (35561 / 36560), FXAA and the world map.
Each is one decompile when its step comes up.

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

## Switch and parity

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
- **The SAO composite's FrameBuffer matrices.** It looks like the IBL and height fog paths of `ISSAOComposite.hlsl` read
  `CameraViewProjInverse` from the raw-loaded engine PerFrame, which the header says holds only valid float4 members. Native mode
  writes the PerFrame values from CS's snapshot; observe mode will show whether the captured matrices were right.
- **Late-built shapes.** A draw the build has no pass for is a tap error today. In native mode the step list comes from the build,
  so an engine draw with no step is invisible unless the inner thunk still checks coverage and logs it.
