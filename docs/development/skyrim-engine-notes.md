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
| 13 (`0x2000`) | ShadowDir | directional shadow applies |
| 14 (`0x4000`) | DefShadow | shadow mask applies |
| 15 (`0x8000`) | ProjectedUV | `kProjectedUV` (bit 23) with snow/material conditions |
| 16 (`0x10000`) | AnisoLighting | `kAnisotropicLighting` (bit 53) |
| 17 (`0x20000`) | AmbientSpecular | decal/fade conditions |
| 18 (`0x40000`) | WorldMap | LOD objects/land with `kMenuScreen` (bit 55) |
| 20 (`0x100000`) | DoAlphaTest | `NiAlphaProperty` alpha test (flag bit 9) and the global alpha-test toggle, or dynamic decals |
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
ambient-specular and lighting-model bits and adds Deferred). As a result, for the opaque deferred pass
the final descriptors depend only on the property flags, the alpha property, the fade state and a few
globals, not on the per-frame light assignment.

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
| 5 | MultiBound | room and multibound nodes (interior statics are parented here, under rooms) |
| 6 | Water | water planes |

Source: the `ToggleCellNode` console command (`ConsoleFunc::handler::ToggleCellNode`, AE `0x14036ef60`,
help string "0-Actor, 1-Marker, 2-Land, 3-Static, 4-Dynamic, 5-Multibound, 6-Water"), which calls
`FUN_14019f8b0(TES, index, show)`. That helper reads `cell3D->children[index]` and toggles bit 0 of
`NiAVObject::flags` (`+0xF4`, `kHidden`/app-culled) for every loaded cell and the sky cell.

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

## Native main-pass render state for opaque lighting objects

Observed at `DrawIndexed` for every eligible draw in the main pass:

-   Draw: `DrawIndexed(triangleCount * 3, 0, 0)` with `rendererData->indexBuffer` (R16, offset 0) and
    `rendererData->vertexBuffer` in slot 0 (stride `vertexDesc.GetSize()`, offset 0).
-   Depth mode `kTestEqual` (the Z-prepass already wrote depth), stencil mode 0, blend mode 0, depth bias 0.
-   Cull mode 0 (none) for `kTwoSided`, 1 (back) otherwise.
-   Alpha test enabled exactly when the `NiAlphaProperty` tests, with reference `alphaThreshold / 255`.
