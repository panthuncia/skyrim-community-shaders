# Skyrim SE/AE engine notes (from the decompiled binary)

Findings from the Ghidra database of `SkyrimSE.exe` 1.6.1170 (AE), gathered while building Drawcall Limit
Fix. Addresses are AE runtime addresses with image base `0x140000000`. Address Library IDs are given as
`SE/AE` where known. Decompiled names such as `FUN_…` and `DAT_…` are Ghidra's; the meanings given are
inferred and should be re-checked before being relied on elsewhere.

## Lighting shader: from property flags to shader descriptors

### `BSLightingShaderProperty::GetRenderPasses` (AE `0x1414adfb0`)

Builds and caches the property's `renderPassList` (the passes stay on the property between frames). The
list is rebuilt only when `lastRenderPassState` changes. That state is `(renderMode << 8) | activeLightMask`,
or the list is forced when light lists change, when the shadow light count changes, or when render mode
`0x16` is used. Render mode `0x1b` takes a separate path.

The main lighting pass stores its technique ID in `BSRenderPass::passEnum` as
`descriptor + 0x4800002D`, where `descriptor` is built as follows (`flags` is the 64-bit
`BSShaderProperty::EShaderPropertyFlag` set):

| Descriptor bits | Meaning | Source |
| --- | --- | --- |
| 0 (`0x1`) | VC | `kVertexColors` (bit 37) |
| 1 (`0x2`) | Skinned | `kSkinned` (bit 1) |
| 2 (`0x4`) | ModelSpaceNormals | `kModelSpaceNormals` (bit 12) |
| 3–5 | point light count − 1 (clamped to 7) | light data |
| 6–8 | shadow light count (clamped to 4) | light data |
| 9 (`0x200`) | Specular | `kSpecular` (bit 0) or `kMultiIndexSnow` (bit 41) |
| 10 (`0x400`) | SoftLighting | `kSoftLighting` (bit 57) |
| 11 (`0x800`) | RimLighting | `kRimLighting` (bit 58) |
| 12 (`0x1000`) | BackLighting | `kBackLighting` (bit 59) |
| 13 (`0x2000`) | ShadowDir | directional shadow applies (light assignment, `FUN_1414fcf80`) |
| 14 (`0x4000`) | DefShadow | shadow mask applies (the accumulator's deferred-shadow flag at `+0x178`, distance and alpha conditions) |
| 15 (`0x8000`) | ProjectedUV | `kProjectedUV` (bit 23) with snow/material conditions |
| 16 (`0x10000`) | AnisoLighting | `kAnisotropicLighting` (bit 53) |
| 17 (`0x20000`) | AmbientSpecular | decal/fade conditions |
| 18 (`0x40000`) | WorldMap | LOD objects/land with `kMenuScreen` (bit 55) |
| 20 (`0x100000`) | DoAlphaTest | `NiAlphaProperty` alpha test (flag bit 9) and either the early-Z global or alpha blending/fading; or decals without `kMultiIndexSnow` (see below) |
| 21 (`0x200000`) | Snow | snow shader enabled and `kSnow` |
| 22 (`0x400000`) | CharacterLight | `kCharacterLighting` (bit 40) |
| 23 (`0x800000`) | AdditionalAlphaMask | screen-door fade (`kScreendoorAlphaFade` off, fade node fading) |
| 24–29 | technique | see below |

Technique selection (bits 24–29), later tests win:

| Technique | Condition |
| --- | --- |
| 1 Envmap | `kEnvMap` (bit 7) |
| 2 Glowmap | `kGlowMap` (bit 38) |
| 3 Parallax | `kParallax` (bit 11) without `kParallaxOcclusion` (bit 28) |
| 4 Facegen | `kFace` (bit 10) |
| 5 FacegenRGBTint | `kFaceGenRGBTint` (bit 21) |
| 6 Hair | `kHairTint` (bit 18) |
| 7 ParallaxOcc | `kParallaxOcclusion` (bit 28) |
| 8 MTLand | `kMultiTextureLandscape` (bit 14) |
| 19 MTLandLODBlend | `kNoLODLandBlend` (bit 46) |
| 18 LODLandNoise | `kLODLandscape` (bit 33) |
| 13 LODObjects | `kLODObjects` (bit 34) |
| 15 LODObjectHD | `kHDLODObjects` (bit 63) |
| 11 MultilayerParallax | `kMultiLayerParallax` (bit 56) |
| 12 TreeAnim | `kTreeAnim` (bit 61) |
| 14 MultiIndexSparkle | `kMultiIndexSnow` (bit 41) and `kProjectedUV` (bit 23) |
| 16 Eye | `kEyeReflect` (bit 17) |

Bits 6–8, 13, 14 and 20 depend on per-frame engine state rather than on the property:

-   ShadowDir, DefShadow and the shadow light count come from the light and shadow assignment.
-   DoAlphaTest for alpha-tested geometry is `alphaTest && (earlyZ || alphaBlended || fading)`. `earlyZ` is
    the byte at AE `0x14328cc79`: the `ToggleEarlyZ` console command flips it, and two offscreen renders
    set it and clear it around their work (`FUN_1406d2120`, and `FUN_140972590` for mode 1). Whether it
    is set in the main pass therefore depends on what rendered before: after a fresh load it was set in
    Whiterun, after `coc` into Dragonsreach it was clear.

Drawcall Limit Fix takes these bits from the pass the main-camera accumulator holds (see "Batch renderer").

`accumulationHint` (`BSRenderPass+0x1C`) records the batch group the pass goes to: 1 when alpha-blended
and fully faded in, 2/3 for decals, 9/10 for fading, 11 for TreeAnim, 15 for plain opaque with no shadow
work, and so on.

### `BSLightingShader::SetupTechnique` (vtable slot 2, AE `0x1414db810`)

Converts `passEnum` back to a descriptor with `d = passEnum - 0x4800002D`, then:

-   Technique 18 (LODLandNoise) becomes 9 (LODLand) unless a noise global is set; technique 7
    (ParallaxOcc) becomes 0 unless a parallax-occlusion global is set.
-   Vertex descriptor (`FUN_14151c730`): `(d & 0x48007) | ((d & 0x20a00) ? 0x200 : 0) | (d & 0x3f000000)`.
    Only VC, Skinned, ModelSpaceNormals, ProjectedUV, WorldMap, Specular (if Specular, AmbientSpecular
    or RimLighting) and the technique survive, so **light counts never reach the vertex shader**.
-   Pixel descriptor (`FUN_14151c760`): `(d & (0xfffffe05 | ((d >> 1) & 2))) | 1`. Bits 3–8 (light counts)
    are cleared, Skinned is kept only with ModelSpaceNormals, and **VC is always set**.
-   Stores `d` at `BSLightingShader+0x94` (read back by SetupGeometry), then sets per-technique sampler
    address modes and writes the PerTechnique constants.

Community Shaders then rewrites both descriptors in `State::ModifyShaderLookup` (strips shadow, snow,
ambient-specular and lighting-model bits and adds Deferred). The stripped bits still matter: Community
Shaders passes them to the shader in its permutation buffer, and SetupTechnique binds the shadow mask from
them (below).

What SetupTechnique writes (ported in `ConstantEvaluator.cpp` `EvaluateTechnique`):

-   **Sampler filter modes** (`PSTextureFilterMode`): slots 0 and 1 anisotropic (3) always; Envmap adds 4
    and 5, Glowmap 6, Parallax 3. SetupMaterial leaves filter modes alone for the vanilla material types
    (TruePBR's hook sets its own).
-   **VS PerTechnique** (`FUN_1414dfad0`): from the fog property of the current scene graph,
    `BSShaderManager::State` (AE `0x142033060`) → `shadowSceneNode[sceneGraph (+0xC0)]` →
    `ShadowSceneNode+0x220` (`BSFogProperty`):
    -   `FogParam` = `(near / (far − near), 1 / (far − near), power (+0x84), clamp (+0x88))`, or
        `(5000000, 0.1, 1, 0)` when both distances are 0;
    -   `FogNearColor` = `(nearColor (+0x38), BSShaderManager::State::invFrameBufferRange (+0x9C))`;
    -   `FogFarColor` = `farColor (+0x44)`; w is whatever was on the stack.
-   **PS PerTechnique**: `FogColor` = `FogNearColor`; `ColourOutputClamp` = `(fLightingOutputColourClampPostLit,
    PostEnv, PostSpec, 0)` (`[General]` settings at AE `0x142035498`, copied into globals at `0x14338ca0c`
    when the shader is constructed, `FUN_1414dad00`).
-   **Shadow mask** when `(ShadowDir || shadow lights) && DefShadow`: t14 = the shadow mask render
    target's SRV, address mode 0, filter mode `iShadowMaskQuarter:Display != 4 ? 1 : 0`; PS `VPOSOffset` =
    `(1 / width, 1 / height, 0, 0)` of the globals at AE `0x14328be9c` (the shadow mask size).
    **The VPOSOffset write happens after the buffer is unmapped.** On DXVK the dynamic buffer stays mapped,
    so the draw still sees it.

### `BSLightingShader::SetupGeometry` (vtable slot 6, AE `0x1414dd040`)

Per-technique constant writes (constant indices come from the shader's reflected `constantTable`):

-   Envmap/MultilayerParallax/Eye (1, 11, 16): writes the eye-position constant and a material float at
    material `+0x104`.
-   MTLand/MTLandLODBlend (8, 19): land blend parameters from the geometry's parent cell data.
-   LOD techniques (9, 13, 15, 18): two transform uploads (current and previous).
-   TreeAnim (12): wind parameters from the object's `BSTreeNode`-like parent (`vfunc 0x1f8`).
-   Everything not skinned: World and PreviousWorld from `geometry->world` (`+0x7c`) and the previous
    world (`+0xb0`, or the current one when render flag `0x10` is set).
-   Then the directional light: colour scaled by the image-space HDR multiplier, and direction
    transformed into model space unless the shader works in world space (Community Shaders patches it to
    always use world space).

## Scene graph layout

### Cell 3D category nodes

A loaded cell's 3D root (`TESObjectCELL` → `loadedData->cell3D`, returned by `FUN_1402b86e0`) has a fixed
set of child nodes, indexed by position in `cell3D->children`:

| Index | Node | Contents |
| --- | --- | --- |
| 0 | Actor | actor 3D (skinned bodies, equipped weapons and armor) |
| 1 | Marker | editor markers |
| 2 | Land | terrain (`BSLightingShader` MTLand) |
| 3 | Static | static references |
| 4 | Dynamic | movable references (Havok clutter, doors, activators) |
| 5 | MultiBound | multibound nodes |
| 6 | Water | water planes |

Source: the `ToggleCellNode` console command (`ConsoleFunc::handler::ToggleCellNode`, AE `0x14036ef60`,
help string "0-Actor, 1-Marker, 2-Land, 3-Static, 4-Dynamic, 5-Multibound, 6-Water"), which calls
`FUN_14019f8b0(TES, index, show)`. That helper reads `cell3D->children[index]` and toggles bit 0 of
`NiAVObject::flags` (`+0xF4`, `kHidden`/app-culled) for every loaded cell and the sky cell.

### Multibounds, rooms and portals: where references really hang

References start under the category nodes, but the engine moves many of them elsewhere, and those are
still drawn by the main pass. Everything below is under `TES::objRoot` (`TES+0x80`, named
`ObjectLODRoot`):

-   **Exteriors:** `objRoot` → `BSMultiBoundNode` (no user data) → `NiNode` → the reference's `BSFadeNode`.
    In Whiterun this held about 60% of the eligible static draws (walls, houses, rocks). It is not the
    cell's `loadedData->multiBoundNode`.
-   **Interiors with a portal graph:** the interior cell's `cell3D` is itself a `BSMultiBoundNode` child of
    `objRoot`. The rooms (`BSMultiBoundRoom`, also listed in `loadedData->portalGraph->rooms`) hang under a
    plain `NiNode` child of it, as does `NiNode 'Shared Portal Geometry'` → `BSPortalSharedNode 'Portal
    Shared Geometry'` (`portalGraph->portalSharedNode`). References are moved into their room, including
    actors.
-   Some interior geometry is drawn with **no parent at all** (about 30 draws per frame in Dragonsreach and
    Bleak Falls Barrow), probably the portal graph's `alwaysRenderChildren` or its other object lists.
    Not confirmed.

### `NiNode` child management overrides

`AttachChild` (vtable 0x35) is overridden by `BSFaceGenNiNode`, `NiSwitchNode`,
`BSParticleSystemManager` and `UI3DSceneManager`. The detach and `SetAt` slots are overridden by
`BGSDecalNode`, `NiSwitchNode` and `BSParticleSystemManager`. Every other node class (`BSFadeNode`,
`BSMultiBoundNode`, `BSLeafAnimNode`, …) uses the `NiNode` implementations, so detouring those catches
attachments for static scene content.

## LOD fades in `GetRenderPasses`

Before choosing the technique, `GetRenderPasses` fades some features out by distance. It compares the
property's fade node LOD metric, a float at `BSFadeNode+0x144` (the previous frame's at `+0x148`), with
`[LightingShader]` INI thresholds through `FUN_14147c470(metric, previous, start, end, &fade, &state)`:

-   If `end < metric`, the feature is off (the function returns false).
-   If `metric <= start`, `fade = 1`. Otherwise `fade = clamp((metric - end) / (start - end), 0, 1)`.

| Feature | Thresholds (default) | Effect when off | Fade stored at |
| --- | --- | --- | --- |
| Specular (`kSpecular` or `kMultiIndexSnow`) | `fSpecularLODFadeStart/End` (0.09 / 0.10) | clears `kSpecular`, so descriptor bit `0x200` is not set | property `+0x100` (`specularLODFade`) |
| Environment map (`kEnvMap`) | `fEnvmapLODFadeStart/End` (0.09 / 0.10) | clears `kEnvMap` unless `kSnow` is set, so technique Envmap becomes None | property `+0x104` (`envmapLODFade`) |
| Decals (`kDecal`/`kDynamicDecal`, not `kZBufferWrite`) | `fDecalLODFadeStart/End` (0.05 / 0.06) | multiplies the object fade | local |
| Refraction (`kRefraction`/`kTempRefraction`, not `kNoFade`) | `fRefractionLODFadeStart/End` (0.025 / 0.03) | no passes at all | property `+0x104` |

The settings are engine `Setting` objects (AE `0x142033370` onward, 0x18 bytes each: vtable, value, name).
`FUN_1414acf90` copies them into the working globals at AE `0x142033350` to `0x142033368`. Read them by
name with `RE::GetINISetting("fSpecularLODFadeStart:LightingShader")`.

Specular and envmap fades are therefore per-frame inputs to the shader descriptors: in Whiterun about 4%
of the eligible main-pass draws had specular faded out.

## Renderer shadow state and shader constant groups

`BSGraphics::RendererShadowState` (AE base `0x14202ab70`, CommonLib `RendererShadowState::GetRuntimeData`)
is the engine's CPU-side copy of the D3D11 state. The fields the Lighting shader's `Setup*` functions use:

| Offset | Field | Notes |
| --- | --- | --- |
| `+0x04` | `PSResourceModifiedBits` | dirty bit per pixel texture slot |
| `+0x08` | `PSSamplerModifiedBits` | dirty bit per sampler slot |
| `+0xbc` | `PSTextureAddressMode[16]` | the material's `textureClampMode` (`+0x70`) for material textures |
| `+0xfc` | `PSTextureFilterMode[16]` | set per technique by SetupTechnique (and by TruePBR) |
| `+0x140` | `PSTexture[16]` | `NiSourceTexture->rendererTexture (+0x48)->srv (+0x10)` |
| `+0x348` / `+0x350` | `currentVertexShader` / `currentPixelShader` | `BSGraphics::VertexShader` / `PixelShader` |
| `+0x35c` / `+0x368` | `posAdjust` / `previousPosAdjust` | camera-relative origin, subtracted from world translations |

Texture binds (`FUN_1414e0460(slot, texture, material)`, and `FUN_1414e06b0` for the envmap cube in slot 4)
only write this state; `SetDirtyStates` later flushes it to D3D11.

Constants go through each shader's three constant groups (`PerTechnique`, `PerMaterial`, `PerGeometry`):
`constantBuffers[level] = {ID3D11Buffer*, void* data}` and a `constantTable` that gives each variable's
offset **in floats** (read as an unsigned byte). The Setup functions map the group's buffer with
`WRITE_DISCARD` (only if the buffer pointer is non-null), write `data + table[i] * 4`, unmap, and bind with
`VS/PSSetConstantBuffers(level, …)`. Variable indices are the engine's; Community Shaders reflects its own
shaders into the same index space (`ShaderCache.cpp` `GetVariableIndices`; a variable a permutation lacks
gets offset 0). The engine sometimes writes only part of a float4 (xy of `EnvmapData` and
`LightingEffectParams`, three floats of `EmitColor`); the rest of a discard-mapped buffer is undefined.

### Evaluating the Setup functions outside the render loop

Because the Setup functions reach D3D11 only through the constant-group pointers, they can be run for any
material or pass without drawing:

1.  Point `currentVertexShader`/`currentPixelShader` at stand-in shader objects whose constant buffers are
    null (so nothing is mapped) and whose `constantTable` sends variable *i* to a known float offset of a
    scratch block pre-filled with a sentinel.
2.  Set `BSLightingShader::currentRawTechnique` (`+0x94`) to the pass descriptor.
3.  Call `SetupMaterial` (vtable slot 4) or `SetupGeometry` (slot 6) through the vtable, so every hook on
    them runs too.
4.  Read the scratch blocks and `PSTexture`/`PSTextureAddressMode`, then restore the whole shadow state,
    `currentRawTechnique` and the D3D11 constant buffer bindings of that level (the functions end with
    `VS/PSSetConstantBuffers(level, 1, &nullBuffer)`).

`SetupGeometry` needs a pass whose `numLights > 0`: it dereferences `sceneLights[0]` (the sun) without a
check. `SetupTechnique` cannot be run this way, because it binds real shaders through `BeginTechnique`.

### `SetupGeometry`: what is per object

With Community Shaders' world-space patch (`Hooks.cpp`: `and r15d, 0` at AE `SetupGeometry+0x71`, which
zeroes the model-space flag), everything `SetupGeometry` writes is per frame except:

| Variable | Value |
| --- | --- |
| VS `World` | row-major 3×4 of `geometry->world`: rotation × scale, translation − `posAdjust` (`FUN_1414aaf10` then `FUN_1414df170`) |
| VS `PreviousWorld` | same from `previousWorld` (or `world` when render flag `0x10` is set), minus `previousPosAdjust` |
| PS `MaterialData` | x = property `envmapLODFade` (Envmap/MultilayerParallax/Eye only), y = `specularLODFade` (Specular only), z = property `alpha` (× fade node `+0x14c` for LOD fade passes) |
| PS `EmitColor` | `*property->emissiveColor × property->emissiveMult` |
| PS `SSRParams.w` | `specularLODFade` when Specular, times 0 if render flag `0x2` |
| PS light counts, point lights, shadow mask select | per-object light assignment; not read by the Light Limit Fix shaders |

The main (deferred) pass calls it with render flags `0x41` or `0x45`.

## Batch renderer: what the main pass will draw

The main-camera `BSShaderAccumulator` (`currentAccumulator`, batch renderer at `+0x130`) holds every pass
the main pass draws; walking it at the start of the main pass gives exactly the passes drawn this frame.

-   `BSBatchRenderer::renderPass` (`+0x08`) is an array of **`PassGroup` structs inline** (0x30 bytes:
    `passes[5]`, `validPassBits`), not the array of pointers CommonLib declares. The engine indexes it
    as `data + (list + group * 6) * 8` (`FUN_1414f39c0`).
-   Each `passes[list]` heads a list chained through `BSRenderPass::passGroupNext` (`+0x30`).
-   `renderPassMap` maps each group's technique ID to its index. The engine reads it raw: bucket count at
    `+0x2C`, buckets of `{ uint32 key, uint32 value, entry* next }` at `+0x48`, end sentinel at `+0x38`.
    An empty bucket has `next == nullptr`.
-   The group's technique ID is what `SetupTechnique` receives, and it can differ from the pass's own
    `passEnum`.
-   `RegisterPass` (AE `0x1414f2a20`, list chosen in `FUN_1414f4790`) puts a pass in list
    `(kTwoSided ? 2 : 0) | (NiAlphaProperty alpha-test bit ? 1 : 0)`, or list 4 for
    `kNoTransparencyMultiSample`. `RenderActivePassRange` draws lists 1, 3 and 4 with alpha testing.
-   A geometry group (`geometryGroups[i]`, 16 of them) has a batch renderer of its own (`+0x00`), which
    `RenderBatches` (AE `0x1414b4fe0`) uses when called with a group index.
-   `BSShaderProperty::renderPassList` holds whatever the **last** `GetRenderPasses` call built. That may
    be another camera's (reflections, cubemaps), so it is not a reliable view of the main pass.

Open question: a pass in an alpha-test list whose group technique lacks DoAlphaTest is sometimes drawn with
DoAlphaTest set in its `passEnum` (about 27 draws per frame in Dragonsreach). Nothing on the draw path
between the start of the main pass and `SetupGeometry` was found to set it: not `RenderActivePassRange`,
`SetupAndDrawPass` (AE `0x1414f3dc0`), `FUN_1414f5cf0`, nor any Community Shaders hook.

## Native main-pass render state for opaque lighting objects

Observed at `DrawIndexed` for every eligible draw in the main pass:

-   Draw: `DrawIndexed(triangleCount * 3, 0, 0)` with `rendererData->indexBuffer` (R16, offset 0) and
    `rendererData->vertexBuffer` in slot 0 (stride `vertexDesc.GetSize()`, offset 0).
-   Depth mode `kTestEqual` (the Z-prepass already wrote depth), stencil mode 0, blend mode 0, depth bias 0.
-   Cull mode 0 (none) for `kTwoSided`, 1 (back) otherwise.
-   Alpha test enabled exactly when the `NiAlphaProperty` tests, with reference `alphaThreshold / 255`.
    This is the batch list the pass is in (lists 1, 3 and 4), not the DoAlphaTest descriptor bit.

## Vertex input

When the shadow state's vertex description or vertex shader changes, the renderer looks up an input layout
by `key = currentVertexShader->vertexDesc & shadowState.vertexDesc` (AE `FUN_140e4b5b0`, shadow state
+0x340 and +0x348) in a hash map and builds a missing one with `FUN_140e4bf20`. The key uses the
`VertexDesc` encoding: the stride / 4 in bits 0-3, an offset nibble (in dwords) per attribute `a` at bits
`4a+4`, and presence flags at bits `44+a` (stream 0) and `54+a` (stream 1, dynamic geometry). The shader's
mask sets both flags and the offset nibble of every attribute it consumes (Community Shaders computes it by
reflection in `ShaderCache`). Elements, per attribute present:

| Attribute | Elements | Format |
| --- | --- | --- |
| Position (always emitted, offset 0) | POSITION0 | R32G32B32A32_FLOAT |
| Texcoord0 | TEXCOORD0 | R16G16_FLOAT |
| Texcoord1 | TEXCOORD1 | R16G16B16A16_FLOAT |
| Normal, Binormal, Color | NORMAL0, BINORMAL0, COLOR0 | R8G8B8A8_UNORM |
| Skinning | BLENDWEIGHT0 (+0), BLENDINDICES0 (+8) | R16G16B16A16_FLOAT, R8G8B8A8_UNORM |
| Land data | TEXCOORD2 (+0), TEXCOORD3 (+4) | R8G8B8A8_UNORM |
| Eye data | TEXCOORD2 | R32_FLOAT |
| Instance data | TEXCOORD4-7 (+0, +8, +16, +24), per instance | R16G16B16A16_FLOAT |

Bit 54 doubles as the geometry flag CommonLib calls `VF_FULLPREC`; the builder never emits a half-precision
position. DCLF (`VertexInput.cpp`) ports this table.

## Samplers

The state-apply function (AE `FUN_140e4b5b0`) binds each dirty pixel shader sampler slot `s` with
`PSSetSamplers(s, 1, &samplers[PSTextureAddressMode[s] * 5 + PSTextureFilterMode[s]])`, where `samplers` is a
table of `ID3D11SamplerState*` at AE `0x143288210` (4 address modes x 5 filter modes), just before the
`Renderer` singleton. DCLF copies these states' descriptions (`GpuTextures::Sampler`); the table's Address
Library ID is not known yet.

## Main-pass viewport

The main (deferred) pass draws with the viewport at the origin, the dynamic-resolution size (2560 x 1440 of a
3840 x 2160 target at 1.5x upscaling), and depth range [0, 0.999998]. Anything that has to reproduce its depth
values must use the same range.

## Decals: where the main pass draws them, and with what state

Decompiled from AE 1.6.1170 and then measured with `CS_DCLF_DECAL_PROBE=1`, which records the
`RendererShadowState` fields at every native Lighting draw of a decal property inside the deferred pass
and reads the D3D11 descriptions of the state objects they index.

`BSShaderAccumulator::FinishAccumulatingPreResolveDepth` for render mode 0 is `FUN_1414b2d90`: the whole
opaque G-buffer pass. In order: the Lighting range (`1..0x5c00002f`), geometry group 9, grass
(`0x5c000030..0x5c00005c`), groups 8, 1 and 0, sky, group 13, then with `alphaBlendWriteMode = 10`
**`FUN_1414b3bb0`** and with `alphaBlendWriteMode = 11` **`FUN_1414b3d50`**, then the water stencils.
Community Shaders' `Main_RenderWorld_BlendedDecals` hook wraps the second of those and calls `EndDeferred`
after it, so both decal groups draw **inside the deferred pass, into the G-buffer**.

`BSLightingShaderProperty::GetRenderPasses` gives a Lighting pass of a `kDecal | kDynamicDecal` property
accumulation hint `2 + (alpha < 1 || NiAlphaProperty blending)`. **A hint `h` lands in
`geometryGroups[h + 1]`** (measured: hint 2 draws with the first function's state, hint 3 with the
second's). Hint 4 passes are Utility-shader passes, not Lighting.

| hint | function | groups | depth mode | `rasterStateDepthBiasMode` | write mode | render flags |
| --- | --- | --- | --- | --- | --- | --- |
| 2 | `FUN_1414b3bb0` | 3 then 2 | 3 (test and write) | `6 + b` while the `ToggleDepthBias` byte at `0x142032ff6` is set, else 0 (group 3); `8 + b` (group 2) | 10 | 0x41 |
| 3 | `FUN_1414b3d50` | 4 | 1 (test only) | `10 + b`, unconditionally | 11 | 0x45 |

`b` is `DrawWorld::disableSunShadows` (`+0x51`), so interiors use modes 7 and 11. The rasterizer states:
modes 6 and 10 hold `DepthBias -1, DepthBiasClamp -100, SlopeScaledDepthBias -0.65`; modes 7 and 11 hold
no bias at all.

Render flag `0x4` is what applies the geometry's `NiAlphaProperty` (`FUN_1414f5cf0` calls
`FUN_14150bc80(shader, alphaProperty, shaderProperty, alphaTest)` before `SetupGeometry`), so it applies
to hint 3 and not to hint 2. `FUN_14150bc80` sets `alphaBlendMode` from the blend functions when the
property blends: 1 for `SrcAlpha / InvSrcAlpha`, 2 for `SrcAlpha / One`, `One / One` and
`SrcAlpha / InvDestAlpha`, 3 for `DestColor / InvSrcAlpha`, 4 for `Zero / SrcColor` and
`DestColor / Zero`; any other pair leaves the mode as it was. With no blending and `shaderProperty->alpha`
below 1 it picks mode 1 (a fading object). It sets `alphaTestEnabled` from the property's test flag.

`BSLightingShader::SetupGeometry` (`Func6`) treats a decal pass specially in one place: for hint 3 with
`kZBufferWrite` it saves the write mode to `0x142035488` and sets **1**; `RestoreGeometry`
(`1414de3d0`, unnamed in Ghidra) puts the saved value back unless the slot holds its sentinel 13. For a
non-decal pass SetupGeometry is what drops the depth mode to 1 without `kZBufferWrite` and to 0 without
`kZBufferTest`; decal passes keep the group function's depth mode.

The blend states (indices `[alphaBlendMode][alphaToCoverage][writeMode][extra]`, the table at
`RelocationID(524749, 411364)`), as Community Shaders' deferred override leaves them: `[0][0][10][0]` is
blending off on every target with mask F; `[1][0][1][0]` is `SrcAlpha / InvSrcAlpha` on every target with
mask 7 on RT0 and RT3-7 and F on RT1-2; `[1][0][11][0]` is the same blend with mask F everywhere. The
only difference between write modes 1 and 11 is therefore RT0's alpha channel, which the deferred
composite never reads (it writes 1.0 into it).

Measured, Whiterun exterior, per frame: 137.5 hint-2 draws alpha-tested without `kZBufferWrite` plus a
few with; 40.7 hint-3 draws blended and alpha-tested with `kZBufferWrite`, 11.8 blended only without it,
5.9 blended only with it. Dragonsreach: 17 hint 2, 13 hint 3. About 0.5% of hint-3 draws without
`kZBufferWrite` were issued with write mode 1 rather than 11: Community Shaders' Terrain Blending sets
`alphaBlendWriteMode = 1` for its own passes from the same hook, just before the blended decals, and the
first decals after it inherit that until a `kZBufferWrite` decal's save and restore resets it.

## Skinning: the palette the engine keeps, and the buffers a skinned draw uses

Decompiled from AE 1.6.1170 for Drawcall Limit Fix's skinned coverage, and measured with
`CS_DCLF_SKIN_PROBE=1`.

**The draw.** `SetupAndDrawPass` (`FUN_1414f3dc0`) takes a different branch when `geometry->skinInstance`
(`RUNTIME_DATA+0x10`) is set: after the shader's `SetupGeometry` it calls the skin instance's vtable slot
`0x25` (`NiSkinInstance::Func37`, `140d451f0`), which walks `skinPartition->numPartitions` and calls
`NiSkinPartition::Unk_25(args, i)` (`140d43a10`) for each. `BSDismemberSkinInstance::Unk_25` (`140d31f40`)
does the same but skips the partitions whose dismember flag is clear.

**Per partition** (`140d43a10`): a partition whose LOD byte (`Partition+0x42`, CommonLib's `pad42`) is not
enabled in the table at `0x14202a030` for the pass's LOD mode is skipped. Otherwise it calls the shader's
bone setter `NiBoneMatrixSetterI::Func1(skinInstance, &partition, &geometry->world)` and draws
**`partition->buffData`**, the partition's own `BSGraphics::TriShape`, for `partition->triangles`
triangles (renderer vtable `+0x38`, or the instanced `+0x30`). `geometry->rendererData` is a different
TriShape and is not what a skinned shape draws (measured: never equal).

**The setter** (`14150be30`): once per (skin instance, thread) it runs the palette update below, then
copies `skinData->bones * 3` float4 rows from `NiSkinInstance::boneMatrices` (`+0x48`) into a dynamic
constant buffer bound at **VS b10** (`Bones`) and `prevBoneMatrices` (`+0x50`) into **VS b9**
(`PreviousBones`). It is the whole skin's palette, whichever partition is being drawn.

**The update** (`FUN_140e4ff90(skinInstance, const NiTransform* world)`): under the instance's critical
section (`+0x60`), only when `frameID` (`+0x38`) differs from `gFrameCounter`: copies the current palette
to the previous one, (re)allocates `bones * 48` bytes when needed, sets `numMatrices = bones` and
`numRegisters = 3`, builds skin-to-world from `rootParent->world` (`+0x20`), `skinData->rootParentToSkin`
and the world passed in, and writes for every bone with a `boneWorldTransforms[i]`:
`boneWorld * skinToBone`, as **three float4 rows** - row-major 3x4, absolute world space, scale folded in,
translation in each row's `.w`. Idempotent within a frame, so whoever calls it first does the work; a
draw that is withheld from the batch renderer means nobody does, which is why DCLF calls it itself.

**The shader** (`Common/Skinned.hlsli`, `Lighting.hlsl` SKINNED): `actualIndices = 765.01 * BoneIndices`
(the UNORM byte times 255 times 3 is the row index), `GetBoneTransformMatrix` sums four `float3x4` rows
minus a pivot, and the pivot is `VS_PerFrame` c40 (`BonesPivot`) / c41 (`PreviousBonesPivot`) - measured
equal to `posAdjust` / `previousPosAdjust` on every one of 62,000 draws. The skinned position never reads
`World`; `SetupGeometry` does not write `World` / `PreviousWorld` for a skinned pass, so the buffer holds
whatever the previous draw left, and `GetBoneRSMatrix` builds the normal basis from the same rows.

**What the main pass draws, per frame.** Whiterun exterior: ~200 skinned Lighting draws, all tracked;
the largest class is one-partition `NiSkinInstance` shapes with 4 bones (47, technique 0), then
three-partition shapes with LOD bytes 1/2/0 and technique 12 (29, hint 11: tree-animated, one partition
per LOD level), then actor parts (`BSDismemberSkinInstance`, one or two draws each). Dragonsreach: ~130,
dominated by one-partition books (`Book01a`, 3 bones, hint 15) and banners (2 bones). 191 of 201 palettes
in the exterior and 288 of 328 in Dragonsreach changed between consecutive frames, so a static-pose skip
would buy little.

## ProjectedUV and MTLand: the per-object constants and where they come from

Decompiled from AE 1.6.1170 for Drawcall Limit Fix's coverage of the two techniques; both are
`BSLightingShader::SetupGeometry` (`Func6`, `1414dd040`) work, and one helper.

**MTLand and MTLandLODBlend (techniques 8 and 19): `LandBlendParams`** (VS PerGeometry variable 3).
`xy` are `BSLightingShaderMaterialLandscape::landBlendParams.rg` (the property's material, `+0x108`).
`zw` are a position blended between two `BSShaderManager::State` points minus the geometry's world
translation:

    t  = clamp((State+0x20 - State+0xb8) * (float at 0x141ad2840 / State+0x98), 0, 1)
    z  = lerp(State+0xa8, State+0xb0, t) - world.translate.x
    w  = lerp(State+0xac, State+0xb4, t) - world.translate.y

with `State` at `0x142033060`. When `t` reaches 1 the function also clears the byte at `0x14332a394`.
Nothing else in the case is per object; the six landscape texture sets come from `SetupMaterial`.

**ProjectedUV (descriptor bit 15, any technique but Hair): `TextureProj`** (VS PerGeometry variable 6,
three rows). The projection is a `NiTransform` with a fixed rotation about Z
(`[[0,1,0],[-1,0,0],[0,0,1]]`, from the constant at `0x141abe6a0`), translation `posAdjust`, scale 1,
converted by `FUN_1414aaf10` (NiTransform to row-major 4x4, translation minus `posAdjust`, so it comes
out at zero). For technique 1 (Envmap) the matrix is that projection alone; for everything else the
geometry's world transform is converted the same way, `posAdjust` is added back to its translation (so it
is the absolute world matrix, with one rounding trip), and `D3DXMatrixMultiply(world, projection)`
(`14153d3c8`) gives `M`. `TextureProj` row `r` is **column** `r` of `M`: `(M[0][r], M[1][r], M[2][r],
M[3][r])`.

The same block binds four pixel textures with address mode 3 and filter mode 1: slot 11 from
`BSShaderManager::State+0x48 -> +0x10` and slots 3, 8 and 10 from the three `NiTexture` globals at
`0x14328cc28/30/38` (through `rendererTexture` `+0x48` then `+0x10`), the ones the
`ReloadProjectedUVTextures` console command replaces. DCLF captures the four views from a native
projected draw rather than dereferencing the globals.

**The pixel parameters** (PS PerGeometry variables 12-14) are written by `FUN_1414e00c0(shader,
dynamicData, property, projectedNormals)` at the end of that block, for a static shape from the
**property's** fields:

| variable | value |
| --- | --- |
| 12 `ProjectedUVParams` | `x = (1 - p.a) * p.r`, `z = p.b`, `w = (1 - p.a) * p.g + p.a` from `projectedUVParams` (`+0x10C`); **y is never written** |
| 13 `ProjectedUVParams2` | `projectedUVColor` (`+0x11C`), all four |
| 14 `ProjectedUVParams3` | `(float at 0x142035560, float at 0x142035578, 0, projectedNormals ? 1 : 0)` |

`projectedNormals` is the byte at `0x142035518`, unless render flag `0x8` is set together with the byte
at `0x142035530` (the main pass runs with `0x41` / `0x45`, so it is the first byte alone).

**The tree amplitude's square root, on garbage.** `SetupGeometry`'s TreeAnim case takes the node's
squared distance (`+0x158`) through `0x5f3759df - (bits >> 1)` with an **arithmetic** shift of the bit
pattern. A fern-type node can hold an uninitialised negative value there; the engine's estimate then
overflows in the direction that clamps the amplitude to its maximum, and a logical shift does not. DCLF
reproduces the signed shift.

## Shadow maps: views, descriptors and the Utility passes

Measured with `CS_DCLF_SHADOW_PROBE=1` (`DrawcallLimitFix/ShadowProbe.cpp`) on AE 1.6.1170, Whiterun
exterior by day and the Bannered Mare at hour 22, on top of the Ghidra reading of the functions below.

**Frame order.** `Main::Draw` (`0x1406444b0`) builds the main scene lists and finishes them; then
`NiCamera::CalculateAndDrawShadowCasterLights` (`0x1414cbb90`) queues each shadow light's list
accumulation and, inside `CalculateActiveShadowCasterLights` (`0x1414cc570`), walks
`ShadowSceneNode::GetShadowCasterLightArrayEntry` (`0x1414a4010`, an index into `shadowLightsAccum` at
`+0x230`) calling `UpdateCamera` (vfunc 0x10) and `Accumulate` (vfunc 0x9, one `FUN_1414f0920` cull per
descriptor). `FUN_1414cbff0`, the callee of Community Shaders' `Main_RenderShadowMaps` thunk
(`RelocationID(35560,36559)+0x2EC/0x30A`), then queues the **main camera's pass registration jobs** and,
while they run, calls every light's `Render` (vfunc 0xA), and only then `JobList__Finish`es. So at
shadow-draw time the scene graph and the main cull are final, the shadow views' registrations are
complete, and the main accumulator's passes are not - they are complete when the thunk returns.

**Per light.** `BSShadowDirectionalLight::Render` (`0x141511d60`), `BSShadowFrustumLight::Render`
(`0x14151aac0`) and `BSShadowParabolicLight::Render` (`0x14151bd10`) loop their `ShadowmapDescriptor`s
(0xF0 bytes, `RE::BSShadowLight::ShadowmapDescriptor`) into `BSShadowLight::RenderShadowmap`
(`0x1414f0cf0`, mislabelled `BSShadowParabolicLight::sub`): a free array slice from the bitmask at
`0x142035797` when `renderTarget == -1`, `SetDepthStencilTarget(descriptor.renderTarget, slice)`, a clear
when `clearRenderTarget`, then `FUN_1414a90f0(camera, accumulator, flags | 0x400)`:
`State::SetCameraData` (the shadow camera into `cameraData` and `posAdjust`), `UpdateViewPort`, the
accumulator's vfunc 0x25, **`FinishAccumulatingPreResolveDepth` (vfunc 0x2A)** and `PostResolveDepth`
(0x2B); afterwards it writes `descriptor.lightTransform` (what CS uploads to t98). Parabolic lights set
`0x142035df8` (the light radius) and `0x142035dfc` (+1 / -1) before each hemisphere.

**The views, as drawn.**

| Light | Descriptors | Render mode | Depth target / slice | Viewport |
|---|---|---|---|---|
| Directional (sun) | 2 cascades | `0xE` ShadowMapClamped | 2 (`kSHADOWMAPS_ESRAM`), slices 0 and 1 | `(0,0) 4096x4096`, full slice |
| Directional, focus shadow | descriptor 0 of `focusShadowmapDescriptors` when `drawFocusShadows` | `0xE` | 4, slice 4 | `(0,3547) 549x549` - **an origin** |
| Parabolic (point) | 2 hemispheres | `0xF` ShadowMapPb | 4, one slice per light | `(0,0) 4096x2048` and `(0,2048) 4096x2048` - **an origin** |
| Frustum (spot) | (none in the cells measured) | `0xE` by the decompile | | |

`ShadowMapPlain` (`0xD`, technique bits `0x4000`) was never used. The `shadowLightsAccum` list holds a
second directional light whose descriptors are never drawn, and the focus descriptors 1..3 have a zero
port and no clear. A view's `port` (`NiRect`: left, right, top, bottom, y up) matches the viewport's
size; the hemispheres and the focus shadow are sub-rectangles of the slice, so any render pass that
draws a view needs a viewport origin, not just a size. Render flags at the hook are `0x400`. The
depth-stencil set mode is 4 (no clear at bind; the clear was done before). Point lights: `parabola =
(radius, ±1)`, e.g. 579 for a hearth. The main pass's `RenderDepth` accumulations (mode `0xC`) come
through the same vfunc twice per frame.

**The passes.** `BSLightingShaderProperty::GetRenderPasses_ShadowMapOrMask` (`0x1414af030`, from
`GetRenderPasses` vfunc 0x2A for modes 0xC..0xF): `technique = DetermineUtilityShaderDecl()` (vfunc
0x3D) `| 0x80` when the alpha property alpha-tests `| 0x2000` (0xC) / `0x4000` (0xD) / `0xC000` (0xE) /
`0x14000` (0xF) `| 0x10000` when LOD-dissolving (0xC only) `| 0x8000000` when flags bit 34 or 63
`| 0x20080` when flags bit 26 or 27; `passEnum = technique + 0x2B`, cached per property in the
double-buffered `arrayQueue` across views of a frame. **For Utility passes the `techniqueID` handed to
`BSBatchRenderer::RegisterPass` is the `passEnum` itself** (unlike Lighting, where it is the descriptor).
The probe derived the technique from the property for ~1M registrations with **0 differing**.
Accumulation hints seen: 0 (most), 3, 7, 11. Exterior day: ~2,680 Utility registrations per frame into
the views (216 cascade 0, ~2,450 cascade 1, 9 focus) and ~2,450 `0x2000`-bit (RenderDepth) ones into a
renderer that is neither a view's nor one of the main pass's batch renderers.

**Who casts.** For the shadow modes the function returns no pass when: the property is not a lighting
one; hair-tint flag (bit 18) with a decal flag (26/27) unless `kZBufferWrite` (32) and alpha-blended;
`fadeNode->currentFade * material->materialAlpha < 1`; flags `0x8004` (`kTempRefraction`,
`kRefraction`); alpha-blended (non-decal); `kCastShadows` (bit 9) clear while the byte at `0x142033498`
is 1 (it is 1 here; 2 selects volumetric copies for 0xE with the accumulator's `+0x12E` flag);
`DetermineUtilityShaderDecl() == 0`. The rule reproduced in the probe rejected **none** of the engine's
registered casters. Of the main pass's kept objects without a registration in a cascade (~475 per
frame in the exterior), 40 were alpha-blended and the rest eligible - outside the cascades' cull.

**The constants** (`BSUtilityShader::SetupGeometry` `0x1414fae40`, `SetupMaterial` `0x1414faa60`,
`SetupTechnique` `0x1414fa160`; `package/Shaders/Utility.hlsl`). VS `PerGeometry` (b2): `ShadowFadeParam`
c0, `World` c1..c4 (row-major, translation in column 3 - the Lighting layout - and **eye-relative to
the shadow camera's `posAdjust`**: `FUN_1414aaf10` subtracts the current `posAdjust`, which the
directional camera puts up to 15,000 units from the main eye), `EyePos` c5, `TreeParams` c7. VS
`PerMaterial` (b1): `TexcoordOffset` c0 (`texCoordOffset`, `texCoordScale`), bound only for alpha-tested
draws. VS `PerTechnique` (b0): `HighDetailRange` c0, `ParabolaParam` c1 = `(1 / radius, ±1)`. The pixel
shader is bound only for alpha-tested draws, and its `PerGeometry` group is **not** - the alpha
reference reaches it as `AlphaTestRefRS` (b11) from `RendererShadowState.alphaTestRef`. `VS_PerFrame`
(b12) `c8..c11` holds the **transpose** of `cameraData.viewProjMat` at the 0x2A hook, and `posAdjust`
at the draw equals the one at the hook. Rasterizer state per draw: cull modes 0 and 1 (two-sided and
back), fill 0; the cascade depth-bias mode is read off the draws in S2.

**Cost.** Exterior day: 2.3 ms of render-thread CPU per frame inside the shadow-map call (max 3.6),
all in the directional light: cascade 1 with 2,569 draws takes 2.2 ms (0.86 µs per draw), cascade 0
with 236 draws 0.23 ms, the focus view 0.014 ms. Bannered Mare at night: two point lights, 4
hemispheres, ~1,500 draws, 0.5 ms.

**The volumetric copy (measured, S2).** Each cascade's accumulator is finished twice per frame: once
with `depthStencil = kSHADOWMAPS_ESRAM` (target 2, 4096², two slices) and once with
`kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM` (target 3, 512², two slices, `D16_UNORM` like the others) for
the volumetric lighting. Both draws walk the same batch renderer, i.e. the same registered passes, so a
pass withheld from that renderer is absent from both maps. `FinishAccumulatingPreResolveDepth` (vfunc
0x2A) fires for each, with the same view (accumulator) and render mode 0xE.
