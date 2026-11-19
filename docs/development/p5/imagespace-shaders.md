# P5.6 ImageSpace shaders: a CS source for every slot

Every ImageSpace shader needs CS HLSL before the ImageSpace chain can run on ORG ([org-full-port.md](../org-full-port.md)
P5.6, [engine-d3d11-surface.md](../engine-d3d11-surface.md) R4). This is the shader-source half: the sources, how each was
checked against the vanilla shader, and the switch that runs them. It does not move any pass to ORG.

## Result

All 124 shader slots now have a CS source (the other 35 slots are composites or empty).

| Before | Slots | Now |
| --- | --- | --- |
| Active (CS HLSL ran) | 39 | Unchanged. Checked against vanilla too (table); several differ by CS's own changes. |
| Source, descriptor commented out | 68 | 45 match vanilla, 20 fixed, 3 differ and stay out of `all` (ISMap, ISTemporalAA_UI, ISTemporalAA_Water). |
| R4 "DXBC only", but a source exists | 3 | The lighting composites: the engine names their file `ISLigthingComposite`. Fixed, exact. |
| No source | 14 | Transcribed from the vanilla DXBC; each matches vanilla on the GPU. |

R4's list of 17 without a source holds 14 real ones: FXAA, BSLensFlare, CopySubRegionCS, the Downsample CS pair, PerlinNoise CS
x2, SAOBlur H/V CS, SAOCameraZAndMipsCS, SAORawAO, SAORawAONoTemporal, SAORawAOCS and SILRawInd. The other three
(LightingComposite x3) had `ISLightingComposite.hlsl` all along.

**On by default (no switch).** The 82 checked slots build from CS's HLSL (`src/ShaderCache.cpp`, `CheckedImagespaceShaders`):
the in-game A/B (`is-sources-0` against `is-sources-1`, 2026-11-10) matched, so `CS_IMAGESPACE_SOURCES` was removed and the
68 commented-out descriptor entries with it. ISMap and the two TAA variants stay on vanilla's DXBC (`enabled = false`, with the
reason beside each). The 39 active slots stay in the old map. ShaderCache now resolves an image-space source file through one helper (`GetImagespaceSourceName`),
which maps BSLensFlare to `ISLensFlare.hlsl` and the misspelled `ISLigthingComposite` to
`ISLightingComposite.hlsl`. BSLensFlare is told by its name (+0x120): its constructor never sets `originalShaderName`
(+0x128), so that field holds leftover heap bytes (an AE crash dump showed `0x001000f208000036`) and is never read for it.
Its fxp name (+0x88) is `LensFlare`; its cache key uses `ISLensFlare` instead.

The A/B scenarios (`is-sources-0.env`, `is-sources-1.env`) went with the switch. FXAA runs only with the game's FXAA setting on.

## How each source was checked

**The vanilla DXBC.** `Skyrim - Shaders.bsa` holds one file, `shadersfx/shaders011.fxp` (the AE 1.6.1170 game folder). It is
the stream `BSShader::LoadShaders` (SE `0x14131f810`) reads in creation order: per raster shader a VS count and a PS count,
then the shaders; per compute shader (`BSComputeShader` load, SE `0x14132f630`) one count, then the shaders. Each shader is
`0x11223344`, the DXBC size, and a header:

| Stage | Header after magic and size (`BSGraphics::Renderer::Load*Shader`, SE `0x140d6f6e0` / `0x140d6fab0` / `0x140d6fda0`) |
| --- | --- |
| VS | id (4), vertex description (8), constant table (20 bytes), buffer sizes (4) |
| PS | id (4), constant table (64 bytes), buffer sizes (4) |
| CS | id (4), constant table (32 bytes), buffer sizes (4) |

The file holds the nine BSShader types' groups (BloodSplatter first), then one group per ImageSpace shader in the order
`CreateEffects` (SE `0x141298480`) constructs them, then two compute groups not mapped here. That order put every compute
group on a compute ctor, and 46 slots compiled to exactly vanilla's bytecode, which confirms the mapping. The AE file was
mapped with the SE ctor order; the shaders are the same.

**The defines** are each shader's `GetShaderMacros` (vtable slot 0x0D; the SE vtables in R4), read by disassembling the
stores into the macro array. They are in the table.

**The constants.** The constant table gives, per name index, the name's offset in floats in b2 (raster). The names are the
ctor's lists: VS names start with `GeometryOffset`, which the base ctor adds and the setup (SE `0x14130c980`, Ghidra calls it
`GetTechniqueName`) writes as (0, 0, 1/width, 1/height) before the shader's own names. CS fills the same table by reflection,
by name (`ReflectConstantBuffers`), so a source is right only if each engine name sits where vanilla reads it. Compute shaders
have empty tables: their constants are one struct uploaded whole into b0 (`SetComputeShaderConstants`, bound by
`DispatchCSShader` at b0), so only the layout matters.

**Three checks per stage**, with vanilla's defines and no feature defines:

1. FXC (`/O3`, as CS builds) against the vanilla disassembly: signatures, declarations, instruction stream, and the constant
   names against the table.
2. A GPU run (D3D11, RX 6600): both shaders on the same random textures and constants, a vertex shader generated from the pixel
   shader's input signature, three seeds, outputs compared to 1e-4 relative. Compute shaders get sizes that make them write.
   "GPU equal" in the table means no output differs. Raster shaders whose vanilla VS has no `SV_Position` (below) cannot run
   this way and were read by hand.
3. DXC for SM 6.6, DXIL and SPIR-V (`-fvk-t-shift 0 0 -fvk-s-shift 100 0 -fvk-u-shift 200 0 -fvk-b-shift 300 0`, then
   spirv-val), under HLSL 2018 (what CS's DXC path uses) and 2021. Every touched or new source passes.

`verify-shader-refactor.ps1` showed the fixes left the other builds of each file unchanged (ISCopy, ISBlur, ISBlurCS's
reflection blur, ISReflectionsRayTracing).

**The transcriptions** were made mechanically from the disassembly with hex literals (`D3DDisassemble` with
`D3D_DISASM_PRINT_HEX_LITERALS`, so constants keep every bit). Temporaries are `uint4` holding raw bits, as DXBC registers do;
float work goes through `asfloat`, integer work through `asint`/`asuint`. Each instruction is one assignment from a vector
constructor, so sources are read before the destination is written. Resources and constants carry the engine's names at
vanilla's registers. The first attempt kept float temporaries: FXC treated small integers stored as floats as denormals and
folded them to zero, and the 6-digit literals of a plain disassembly changed the Perlin hash; both are why the method is as
described. The ISBlurCS and ISLensFlare/CopySubRegion sources were written by hand.

## Per slot

"Before" is R4's status. The SE / AE columns are the ctor IDs. Composites and empty slots are left out.

| # | Effect | Engine name | Source | SE / AE ctor ID | Defines (GetShaderMacros) | Before | Check result |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 2 | ISFXAA | BSImagespaceShaderFXAA | ISFXAA | 99077 / 105724 | - | DXBC only | transcribed; GPU equal |
| 34 | ISCopy | BSImagespaceShaderCopy | ISCopy | 99066 / inlined | - | src-unmapped | matches: exact |
| 35 | ISCopyDynamicFetchDisabled | BSImagespaceShaderCopyDynamicFetchDisabled | ISCopy | 99068 / inlined | DISABLE_DYNAMIC | src-unmapped | matches: exact |
| 36 | ISCopyScaleBias | BSImagespaceShaderCopyScaleBias | ISCopy | 99069 / inlined | SCALE_BIAS | src-unmapped | fixed: VS lacked the ScaleBias transform; now exact |
| 37 | ISCopyCustomViewport | BSImagespaceShaderCopyCustomViewport | ISCopy | 99067 / inlined | - | src-unmapped | matches: exact |
| 38 | ISCopyGrayScale | BSImagespaceShaderGreyScale | ISCopy | 99078 / inlined | GREYSCALE | src-unmapped | matches: exact |
| 39 | ISRefraction | BSImagespaceShaderRefraction | ISRefraction | 99157 / 105791 | - | active | differs on random inputs (CS changes, not reviewed) |
| 40 | ISDoubleVision | BSImagespaceShaderDoubleVision | ISDoubleVision | 99076 / 105723 | - | active | differs on random inputs (CS changes, not reviewed) |
| 41 | ISCopyTextureMask | BSImagespaceShaderTextureMask | ISCopy | 99158 / inlined | MASK | src-unmapped | fixed: VS lacked the RotationMatrix turn; now exact |
| 42 | ISMap | BSImagespaceShaderMap | ISMap | 99140 / inlined | - | src-unmapped | differs: vanilla VS writes POSITION, not SV_Position (its draw has no position, inferred); CS uses SV_Position. PS matches (sum order). Not in `all` |
| 43 | ISWorldMap | BSImagespaceShaderWorldMap | ISWorldMap | 99170 / 105800 | _NOTHING | src-unmapped | fixed: constant names on the wrong registers, taps stepped by multiplying; GPU equal |
| 44 | ISWorldMapNoSkyBlur | BSImagespaceShaderWorldMapNoSkyBlur | ISWorldMap | 99171 / 105801 | _NOTHING NO_SKY | src-unmapped | fixed: as ISWorldMap; GPU equal |
| 45 | ISDepthOfField | BSImagespaceShaderDepthOfField | ISDepthOfField | 99070 / 105717 | - | active | GPU equal |
| 46 | ISDepthOfFieldFogged | BSImagespaceShaderDepthOfFieldFogged | ISDepthOfField | 99071 / 105718 | FOGGED | active | GPU equal |
| 47 | ISDepthOfFieldMaskedFogged | BSImagespaceShaderDepthOfFieldMaskedFogged | ISDepthOfField | 99072 / 105719 | FOGGED MASKED | active | GPU equal |
| 49 | ISDistantBlur | BSImagespaceShaderDistantBlur | ISDepthOfField | 99073 / 105720 | DISTANT | active | GPU equal |
| 50 | ISDistantBlurFogged | BSImagespaceShaderDistantBlurFogged | ISDepthOfField | 99074 / 105721 | DISTANT FOGGED | active | GPU equal |
| 51 | ISDistantBlurMaskedFogged | BSImagespaceShaderDistantBlurMaskedFogged | ISDepthOfField | 99075 / 105722 | DISTANT FOGGED MASKED | active | GPU equal |
| 52 | ISRadialBlur | BSImagespaceShaderRadialBlur | ISRadialBlur | 99150 / 105784 | NUM_STEPS=2 | active | differs on random inputs (CS changes, not reviewed) |
| 53 | ISRadialBlurMedium | BSImagespaceShaderRadialBlurMedium | ISRadialBlur | 99152 / 105786 | NUM_STEPS=6 | active | differs on random inputs (CS changes, not reviewed) |
| 54 | ISRadialBlurHigh | BSImagespaceShaderRadialBlurHigh | ISRadialBlur | 99151 / 105785 | NUM_STEPS=10 | active | differs on random inputs (CS changes, not reviewed) |
| 55 | ISHDRTonemapBlendCinematic | BSImagespaceShaderHDRTonemapBlendCinematic | ISHDR | 99087 / 105733 | BLEND=4 | active | differs on random inputs (CS changes, not reviewed) |
| 56 | ISHDRTonemapBlendCinematicFade | BSImagespaceShaderHDRTonemapBlendCinematicFade | ISHDR | 99088 / 105734 | BLEND=4 FADE | active | differs on random inputs (CS changes, not reviewed) |
| 57 | ISHDRDownSample16 | BSImagespaceShaderHDRDownSample16 | ISHDR | 99079 / 105725 | DOWNSAMPLE=16 | active | GPU equal |
| 58 | ISHDRDownSample4 | BSImagespaceShaderHDRDownSample4 | ISHDR | 99083 / 105729 | DOWNSAMPLE=4 | active | GPU equal |
| 59 | ISHDRDownSample16Lum | BSImagespaceShaderHDRDownSample16Lum | ISHDR | 99081 / 105727 | DOWNSAMPLE=16 LUM | active | GPU equal |
| 60 | ISHDRDownSample4RGB2Lum | BSImagespaceShaderHDRDownSample4RGB2Lum | ISHDR | 99086 / 105732 | DOWNSAMPLE=4 LUM RGB2LUM | active | GPU equal |
| 61 | ISHDRDownSample4LumClamp | BSImagespaceShaderHDRDownSample4LumClamp | ISHDR | 99085 / 105731 | DOWNSAMPLE=4 LUMCLAMP | active | differs on random inputs (CS changes, not reviewed) |
| 62 | ISHDRDownSample4LightAdapt | BSImagespaceShaderHDRDownSample4LightAdapt | ISHDR | 99084 / 105730 | DOWNSAMPLE=4 DOWNADAPT | active | GPU equal |
| 63 | ISHDRDownSample16LumClamp | BSImagespaceShaderHDRDownSample16LumClamp | ISHDR | 99082 / 105728 | DOWNSAMPLE=16 LUMCLAMP LUM | active | GPU equal |
| 64 | ISHDRDownSample16LightAdapt | BSImagespaceShaderHDRDownSample16LightAdapt | ISHDR | 99080 / 105726 | DOWNSAMPLE=16 DOWNADAPT LUM | active | differs on random inputs (CS changes, not reviewed) |
| 65 | ISBlur3 | BSImagespaceShaderBlur3 | ISBlur | 99055 / 105706 | TEXTAP=3 COLORRANGE | src-unmapped | matches: exact |
| 66 | ISBlur5 | BSImagespaceShaderBlur5 | ISBlur | 99056 / 105707 | TEXTAP=5 COLORRANGE | src-unmapped | matches: exact |
| 67 | ISBlur7 | BSImagespaceShaderBlur7 | ISBlur | 99057 / 105708 | TEXTAP=7 COLORRANGE | src-unmapped | matches: exact |
| 68 | ISBlur9 | BSImagespaceShaderBlur9 | ISBlur | 99058 / 105709 | TEXTAP=9 COLORRANGE | src-unmapped | matches: exact |
| 69 | ISBlur11 | BSImagespaceShaderBlur11 | ISBlur | 99052 / 105703 | TEXTAP=11 COLORRANGE | src-unmapped | matches: exact |
| 70 | ISBlur13 | BSImagespaceShaderBlur13 | ISBlur | 99053 / 105704 | TEXTAP=13 COLORRANGE | src-unmapped | matches: exact |
| 71 | ISBlur15 | BSImagespaceShaderBlur15 | ISBlur | 99054 / 105705 | TEXTAP=15 COLORRANGE | src-unmapped | matches: exact |
| 72 | ISNonHDRBlur3 | BSImageSpaceShaderNonHDRBlur3 | ISBlur | 99146 / 105780 | TEXTAP=3 | src-unmapped | matches: exact |
| 73 | ISNonHDRBlur5 | BSImageSpaceShaderNonHDRBlur5 | ISBlur | 99147 / 105781 | TEXTAP=5 | src-unmapped | matches: exact |
| 74 | ISNonHDRBlur7 | BSImageSpaceShaderNonHDRBlur7 | ISBlur | 99148 / 105782 | TEXTAP=7 | src-unmapped | matches: exact |
| 75 | ISNonHDRBlur9 | BSImageSpaceShaderNonHDRBlur9 | ISBlur | 99149 / 105783 | TEXTAP=9 | src-unmapped | matches: exact |
| 76 | ISNonHDRBlur11 | BSImageSpaceShaderNonHDRBlur11 | ISBlur | 99143 / 105777 | TEXTAP=11 | src-unmapped | matches: exact |
| 77 | ISNonHDRBlur13 | BSImageSpaceShaderNonHDRBlur13 | ISBlur | 99144 / 105778 | TEXTAP=13 | src-unmapped | matches: exact |
| 78 | ISNonHDRBlur15 | BSImageSpaceShaderNonHDRBlur15 | ISBlur | 99145 / 105779 | TEXTAP=15 | src-unmapped | matches: exact |
| 79 | ISBrightPassBlur3 | BSImagespaceShaderBrightPassBlur3 | ISBlur | 99062 / 105713 | TEXTAP=3 BRIGHTPASS COLORRANGE | src-unmapped | fixed: average luminance texel not scaled by BlurScale.w; GPU equal |
| 80 | ISBrightPassBlur5 | BSImagespaceShaderBrightPassBlur5 | ISBlur | 99063 / 105714 | TEXTAP=5 BRIGHTPASS COLORRANGE | src-unmapped | fixed: as ISBrightPassBlur3; GPU equal |
| 81 | ISBrightPassBlur7 | BSImagespaceShaderBrightPassBlur7 | ISBlur | 99064 / 105715 | TEXTAP=7 BRIGHTPASS COLORRANGE | src-unmapped | fixed: as ISBrightPassBlur3; GPU equal |
| 82 | ISBrightPassBlur9 | BSImagespaceShaderBrightPassBlur9 | ISBlur | 99065 / 105716 | TEXTAP=9 BRIGHTPASS COLORRANGE | src-unmapped | fixed: as ISBrightPassBlur3; GPU equal |
| 83 | ISBrightPassBlur11 | BSImagespaceShaderBrightPassBlur11 | ISBlur | 99059 / 105710 | TEXTAP=11 BRIGHTPASS COLORRANGE | src-unmapped | fixed: as ISBrightPassBlur3; GPU equal |
| 84 | ISBrightPassBlur13 | BSImagespaceShaderBrightPassBlur13 | ISBlur | 99060 / 105711 | TEXTAP=13 BRIGHTPASS COLORRANGE | src-unmapped | fixed: as ISBrightPassBlur3; GPU equal |
| 85 | ISBrightPassBlur15 | BSImagespaceShaderBrightPassBlur15 | ISBlur | 99061 / 105712 | TEXTAP=15 BRIGHTPASS COLORRANGE | src-unmapped | fixed: as ISBrightPassBlur3; GPU equal |
| 86 | ISWaterDisplacementClearSimulation | BSISWaterDisplacementClearSimulation | ISWaterDisplacement | 99161 / inlined | CLEAR_SIMULATION | src-unmapped | matches: exact |
| 87 | ISWaterDisplacementTexOffset | BSISWaterDisplacementTexOffset | ISWaterDisplacement | 99164 / 105795 | TEXCOORD_OFFSET | src-unmapped | matches (one literal 10.0 vs 10.000001) |
| 88 | ISWaterDisplacementWadingRipple | BSISWaterDisplacementWadingRipple | ISWaterDisplacement | 99165 / inlined | RIPPLE_MAKER_WADING | src-unmapped | fixed: VS lacked the TranslationMatrix transform; now exact |
| 89 | ISWaterDisplacementRainRipple | BSISWaterDisplacementRainRipple | ISWaterDisplacement | 99163 / inlined | RIPPLE_MAKER_RAIN | src-unmapped | matches: exact |
| 90 | ISWaterWadingHeightmap | BSISWaterWadingHeightmap | ISWaterDisplacement | 99169 / 105799 | HEIGHTMAP_WADING | src-unmapped | matches (sum order); GPU equal |
| 91 | ISWaterRainHeightmap | BSISWaterRainHeightmap | ISWaterDisplacement | 99167 / 105797 | HEIGHTMAP_RAIN | src-unmapped | matches (sum order); GPU equal |
| 92 | ISWaterBlendHeightmaps | BSISWaterBlendHeightmaps | ISWaterDisplacement | 99160 / 105793 | BLEND_HEIGHTMAPS | src-unmapped | matches (operand order); GPU equal |
| 93 | ISWaterSmoothHeightmap | BSISWaterSmoothHeightmap | ISWaterDisplacement | 99168 / 105798 | HEIGHTMAP_SMOOTH | src-unmapped | matches: exact |
| 94 | ISWaterDisplacementNormals | BSISWaterDisplacementNormals | ISWaterDisplacement | 99162 / 105794 | NORMALS | src-unmapped | matches (Sobel sum order); GPU equal |
| 95 | ISNoiseScrollAndBlend | BSImagespaceShaderNoiseScrollAndBlend | ISNoise | 99142 / 105776 | SCROLL_AND_BLEND | src-unmapped | matches (operand order). VS writes POSITION, no SV_Position, as vanilla does |
| 96 | ISNoiseNormalmap | BSImagespaceShaderNoiseNormalmap | ISNoise | 99141 / 105775 | NORMALMAP | src-unmapped | matches (Sobel sum order). VS writes POSITION, no SV_Position, as vanilla does |
| 97 | ISVolumetricLighting | BSImageSpaceShaderVolumetricLighting | ISVolumetricLighting | 99159 / 105792 | - | active | exact |
| 98 | ISLocalMap | BSImagespaceShaderLocalMap | ISLocalMap | 99139 / inlined | - | src-unmapped | matches (sum order); GPU equal |
| 99 | ISAlphaBlend | BSImagespaceShaderAlphaBlend | ISAlphaBlend | 99050 / 105701 | - | src-unmapped | matches (lerp form); GPU equal |
| 100 | ISLensFlare | BSLensFlare | ISLensFlare (engine: none) | 100273 / inlined | - | DXBC only | transcribed (ISLensFlare.hlsl); exact |
| 101 | ISLensFlareVisibility | BGSLensFlareVisibilityPass | LensFlare | 100284 / inlined | - | active | differs on random inputs (CS changes, not reviewed) |
| 102 | ISApplyReflections | BSImagespaceShaderApplyReflections | ISApplyReflections | 99051 / 105702 | - | src-unmapped | matches (sample moved into the branch); GPU equal |
| 103 | ISApplyVolumetricLighting | BSImagespaceShaderISApplyVolumetricLighting | ISApplyVolumetricLighting | 99089 / 105735 | - | active | differs on random inputs (CS changes, not reviewed) |
| 104 | ISBasicCopy | BSImagespaceShaderISBasicCopy | ISBasicCopy | 99090 / inlined | - | src-unmapped | matches: exact |
| 105 | ISBlur | BSImagespaceShaderISBlur | ISBlur | 99091 / 105736 | - | src-unmapped | matches: exact |
| 106 | ISVolumetricLightingBlurHCS | BSImagespaceShaderISVolumetricLightingBlurHCS | ISBlurCS | 99136 / 105772 | GRIDSIZE_X=960 TEXTAP=5 BLUR_SCALE=6 SIGMA=3 DEPTH_THRESHOLD=0.002f OUTPUT_CHANNEL_COUNT=1 EARLY_DEPTH_REJECT | src-unmapped | fixed: ISBlurCS had only the reflection blur (build error); same arithmetic + bounds check; GPU equal |
| 107 | ISVolumetricLightingBlurVCS | BSImagespaceShaderISVolumetricLightingBlurVCS | ISBlurCS | 99137 / 105773 | GRIDSIZE_Y=540 TEXTAP=5 BLUR_SCALE=6 SIGMA=3 DEPTH_THRESHOLD=0.002f OUTPUT_CHANNEL_COUNT=1 EARLY_DEPTH_REJECT | src-unmapped | fixed: as 106; GPU equal |
| 108 | ISReflectionBlurHCS | BSImagespaceShaderReflectionBlurHCS | ISBlurCS | 99153 / 105787 | GRIDSIZE_X=960 TEXTAP=9 BLUR_SCALE=2 SIGMA=2 OUTPUT_CHANNEL_COUNT=4 | active | GPU equal |
| 109 | ISReflectionBlurVCS | BSImagespaceShaderReflectionBlurVCS | ISBlurCS | 99154 / 105788 | GRIDSIZE_Y=540 TEXTAP=9 BLUR_SCALE=2 SIGMA=2 OUTPUT_CHANNEL_COUNT=4 | active | GPU equal |
| 110 | ISParallaxMaskBlurHCS | BSImagespaceShaderISParallaxMaskBlurHCS | ISBlurCS | 99111 / 105754 | GRIDSIZE_X=900 TEXTAP=11 BLUR_SCALE=8 SIGMA=18 OUTPUT_CHANNEL_COUNT=1 INPUT_CHANNEL_ALPHA | src-unmapped | fixed: as 106; GPU equal |
| 111 | ISParallaxMaskBlurVCS | BSImagespaceShaderISParallaxMaskBlurVCS | ISBlurCS | 99112 / 105755 | GRIDSIZE_Y=900 TEXTAP=11 BLUR_SCALE=8 SIGMA=18 OUTPUT_CHANNEL_COUNT=1 | src-unmapped | fixed: as 106; GPU equal |
| 112 | ISDepthOfFieldBlurHCS | BSImagespaceShaderISDepthOfFieldBlurHCS | ISBlurCS | 99097 / 105741 | GRIDSIZE_X=480 TEXTAP=5 BLUR_SCALE=1 SIGMA=1 DEPTH_THRESHOLD=0.04f | src-unmapped | fixed: as 106; GPU equal |
| 113 | ISDepthOfFieldBlurVCS | BSImagespaceShaderISDepthOfFieldBlurVCS | ISBlurCS | 99098 / 105742 | GRIDSIZE_Y=270 TEXTAP=5 BLUR_SCALE=1 SIGMA=1 DEPTH_THRESHOLD=0.04f | src-unmapped | fixed: as 106; GPU equal |
| 114 | ISCompositeVolumetricLighting | BSImagespaceShaderISCompositeVolumetricLighting | ISCompositeLensFlareVolumetricLighting | 99094 / 105739 | VOLUMETRIC_LIGHTING | active | exact |
| 115 | ISCompositeLensFlare | BSImagespaceShaderISCompositeLensFlare | ISCompositeLensFlareVolumetricLighting | 99092 / 105737 | LENS_FLARE | active | exact |
| 116 | ISCompositeLensFlareVolumetricLighting | BSImagespaceShaderISCompositeLensFlareVolumetricLighting | ISCompositeLensFlareVolumetricLighting | 99093 / 105738 | VOLUMETRIC_LIGHTING LENS_FLARE | active | exact |
| 117 | ISCopySubRegionCS | BSImagespaceShaderISCopySubRegionCS | ISCopySubRegionCS | 99095 / 105740 | - | DXBC only | transcribed; exact |
| 118 | ISDebugSnow | BSImagespaceShaderISDebugSnow | ISDebugSnow | 99096 / inlined | - | src-unmapped | matches: exact |
| 119 | ISDownsample | BSImagespaceShaderISDownsample | ISDownsample | 99100 / 105744 | PICK_BRIGHTEST COMPENSATE | active | exact |
| 120 | ISDownsampleIgnoreBrightest | BSImagespaceShaderISDownsampleIgnoreBrightest | ISDownsample | 99102 / 105746 | DYNAMIC_SOURCE | active | exact |
| 121 | ISDownsampleCS | BSImagespaceShaderISDownsampleCS | ISDownsampleCS | 99101 / 105745 | PICK_BRIGHTEST GRIDSIZE_X=1024 | DXBC only | transcribed; GPU equal |
| 122 | ISDownsampleIgnoreBrightestCS | BSImagespaceShaderISDownsampleIgnoreBrightestCS | ISDownsampleCS | 99103 / 105747 | GRIDSIZE_X=1024 | DXBC only | transcribed; GPU equal |
| 123 | ISExp | BSImagespaceShaderISExp | ISExp | 99104 / inlined | - | src-unmapped | matches: exact |
| 124 | ISIBLensFlares | BSImagespaceShaderISIBLensFlares | ISIBLensFlare | 99105 / 105748 | - | src-unmapped | fixed: missing saturates, globalIntensity at c4.x not c3.y; now exact |
| 125 | ISLightingComposite | BSImagespaceShaderISLightingComposite | ISLightingComposite (engine: ISLigthingComposite) | 99106 / 105749 | DIRECTIONALLIGHT | src-unmapped (R4: DXBC only) | fixed: missing saturate; now exact. Source existed; the engine name has the typo ISLigthingComposite |
| 126 | ISLightingCompositeNoDirectionalLight | BSImagespaceShaderISLightingCompositeNoDirectionalLight | ISLightingComposite (engine: ISLigthingComposite) | 99108 / 105751 | - | src-unmapped (R4: DXBC only) | fixed: as 125; now exact |
| 127 | ISLightingCompositeMenu | BSImagespaceShaderISLightingCompositeMenu | ISLightingComposite (engine: ISLigthingComposite) | 99107 / 105750 | MENU DIRECTIONALLIGHT | src-unmapped (R4: DXBC only) | fixed: as 125; now exact |
| 128 | ISPerlinNoiseCS | BSImagespaceShaderISPerlinNoiseCS | ISPerlinNoiseCS | 99114 / inlined | - | DXBC only | transcribed; GPU equal |
| 129 | ISPerlinNoise2DCS | BSImagespaceShaderISPerlinNoise2DCS | ISPerlinNoiseCS | 99113 / inlined | TEXTURE2D | DXBC only | transcribed; GPU equal |
| 130 | ISReflectionsRayTracing | BSImagespaceShaderReflectionsRayTracing | ISReflectionsRayTracing | 99156 / 105790 | - | active | differs on random inputs (CS changes, not reviewed) |
| 131 | ISReflectionsDebugSpecMask | BSImagespaceShaderReflectionsDebugSpecMask | ISReflectionsRayTracing | 99155 / 105789 | SPECMASK | src-unmapped | fixed: CS rewrite had no SPECMASK path; transcribed, matches (operand order); GPU equal |
| 132 | ISSAOBlurH | BSImagespaceShaderISSAOBlurH | ISSAOBlur | 99115 / inlined | AXIS_H | src-unmapped | matches (early-out moved); GPU equal |
| 133 | ISSAOBlurV | BSImagespaceShaderISSAOBlurV | ISSAOBlur | 99117 / inlined | AXIS_V | src-unmapped | matches (early-out moved); GPU equal |
| 134 | ISSAOBlurHCS | BSImagespaceShaderISSAOBlurHCS | ISSAOBlurCS | 99116 / 105756 | GRID_SIZE=972 | DXBC only | transcribed; GPU equal |
| 135 | ISSAOBlurVCS | BSImagespaceShaderISSAOBlurVCS | ISSAOBlurCS | 99118 / 105757 | AXIS_V GRID_SIZE=552 | DXBC only | transcribed; GPU equal |
| 136 | ISSAOCameraZ | BSImagespaceShaderISSAOCameraZ | ISSAOCameraZ | 99119 / 105758 | - | src-unmapped | matches: exact |
| 137 | ISSAOCameraZAndMipsCS | BSImagespaceShaderISSAOCameraZAndMipsCS | ISSAOCameraZAndMipsCS | 99120 / 105759 | - | DXBC only | transcribed; GPU equal |
| 138 | ISSAOCompositeSAO | BSImagespaceShaderISSAOCompositeSAO | ISSAOComposite | 99122 / 105761 | APPLY_SAO | active | differs on random inputs (CS changes, not reviewed) |
| 139 | ISSAOCompositeFog | BSImagespaceShaderISSAOCompositeFog | ISSAOComposite | 99121 / 105760 | APPLY_FOG | active | GPU equal |
| 140 | ISSAOCompositeSAOFog | BSImagespaceShaderISSAOCompositeSAOFog | ISSAOComposite | 99123 / 105762 | APPLY_SAO APPLY_FOG | active | GPU equal |
| 141 | ISMinify | BSImagespaceShaderISMinify | ISSAOMinify | 99109 / 105752 | - | active | GPU equal |
| 142 | ISMinifyContrast | BSImagespaceShaderISMinifyContrast | ISSAOMinify | 99110 / 105753 | APPLY_CENTER_CONTRAST | active | GPU equal |
| 143 | ISSAORawAO | BSImagespaceShaderISSAORawAO | ISSAORawAO | 99124 / 105763 | MOTION_VECTORS | DXBC only | transcribed; GPU equal |
| 144 | ISSAORawAONoTemporal | BSImagespaceShaderISSAORawAONoTemporal | ISSAORawAO | 99126 / 105765 | MOTION_VECTORS EDITOR | DXBC only | transcribed; GPU equal |
| 145 | ISSAORawAOCS | BSImagespaceShaderISSAORawAOCS | ISSAORawAOCS | 99125 / 105764 | - | DXBC only | transcribed; GPU equal |
| 146 | ISSILComposite | BSImagespaceShaderISSILComposite | ISSILComposite | 99127 / inlined | - | src-unmapped | matches: exact |
| 147 | ISSILRawInd | BSImagespaceShaderISSILRawInd | ISSILRawInd | 99128 / 105766 | - | DXBC only | transcribed; GPU equal |
| 148 | ISSimpleColor | BSImagespaceShaderISSimpleColor | ISSimpleColor | 99129 / 105767 | - | src-unmapped | matches: exact |
| 149 | ISDisplayDepth | BSImagespaceShaderISDisplayDepth | ISSimpleColor | 99099 / 105743 | DISPLAY_DEPTH | src-unmapped | matches (x*(1/y) vs x/y); GPU equal |
| 150 | ISSnowSSS | BSImagespaceShaderISSnowSSS | ISSnowSSS | 99130 / 105768 | - | src-unmapped | matches (operand order); GPU equal |
| 151 | ISTemporalAA | BSImagespaceShaderISTemporalAA | ISTemporalAA | 99131 / 105769 | - | active | differs on random inputs (CS changes, not reviewed) |
| 152 | ISTemporalAA_UI | BSImagespaceShaderISTemporalAA_UI | ISTemporalAA | 99132 / 105770 | TAA_UI | src-unmapped | differs: CS ISTemporalAA.hlsl is its own rewrite with no TAA_UI path and other constant names. Not in `all` |
| 153 | ISTemporalAA_Water | BSImagespaceShaderISTemporalAA_Water | ISTemporalAA | 99133 / 105771 | TAA_BLEND_WATER | src-unmapped | differs: no TAA_BLEND_WATER path in CS ISTemporalAA.hlsl. Not in `all` |
| 154 | ISUpsampleDynamicResolution | BSImagespaceShaderISUpsampleDynamicResolution | ISUpsampleDynamicResolution | 99135 / inlined | - | src-unmapped | matches: exact |
| 155 | ISWaterBlend | BSImagespaceShaderISWaterBlend | ISWaterBlend | 99138 / 105774 | - | active | differs on random inputs (CS changes, not reviewed) |
| 156 | ISUnderwaterMask | BSImagespaceShaderISUnderwaterMask | Water | 99134 / inlined | UNDERWATERMASK | src-unmapped | matches: exact |
| 157 | ISWaterFlow | BSImagespaceShaderWaterFlow | ISWaterFlow | 99166 / inlined | PAINT_FLOW_DIRECTION | src-unmapped | matches: exact |

"Matches" with a reason in brackets means the bytecode differs only by FXC's choice of order or form, and the arithmetic is the
same. "Differs on random inputs (CS changes, not reviewed)" marks active sources that CS rewrote on purpose (HDR output,
dynamic resolution, its TAA, SSR); this work did not change them.

## Findings outside the slots enabled here

- **Three vanilla vertex shaders write `POSITION`, not `SV_Position`:** ISMap (42), ISNoiseScrollAndBlend (95) and
  ISNoiseNormalmap (96). Their draws have no position, so they probably rasterize nothing _(inferred)_. CS's ISNoise.hlsl
  reproduces this exactly; CS's ISMap uses `SV_Position` and would draw, so ISMap stays out of `all`. The ORG pass has to
  decide whether to draw these at all.
- **CS's active ISTemporalAA.hlsl uses its own constant names** (`TexelSizeParams` ...), not the engine's
  (`InverseSize_MaxUV`, `UVOffset_SampleWeight8_Zero`, `SampleWeights0123`, `SampleWeights4567`,
  `LowFrequency_HighFrequency_Sharpen_Overlay`, `NearFar_Menu_EffectThreshold`), which sit at c0 to c5 in that order. CS
  fills the constant table by name and leaves unmatched names at offset 0, so by the code every TAA constant would land at
  c0 _(not checked in game)_. Its registers are in vanilla's order, so renaming the six members to the engine's names would
  give each its own register; that changes an active shader, so it is not done here. TAA_UI and TAA_Water also need their own
  paths in that file (vanilla's UI build reads the mask from t4 instead of t5; the water build blends a water history).
- **CS's active ISRefraction.hlsl never reads `Tint`** (vanilla multiplies by it), and several active sources do not build
  under HLSL 2021 (ISRefraction, ISHDR, ISTemporalAA: vector conditions in `?:` need `select`). Under 2018 they build.
- **Engine names that differ from CommonLib's or CS's old comments:** the texture mask copy is `BSImagespaceShaderTextureMask`
  (CS's comment said `...CopyTextureMask`), the non-HDR blurs are `BSImageSpaceShaderNonHDRBlur*` (capital S), and CS's map
  entries `BSImagespaceShaderVolumetricLightingBlur[HV]CS` are CS's own shaders, not vanilla's
  `BSImagespaceShaderISVolumetricLightingBlur[HV]CS` (106/107, now in the optional list).
- **CommonLib's `BSImagespaceShader::samplerNames` offset is wrong:** the ctors fill arrays at +0x138 (VS names), +0x150
  (PS names) and +0x168 (sampler and UAV names); CommonLib puts `samplerNames` at +0x158.

## What the ORG ImageSpace pass still needs

- **Bindings stay FXC registers.** The sources are not converted to `Common/OrgResources.hlsli`: the D3D11 path finds raster
  constants by cbuffer member name through reflection, so the names and registers must stay as they are, and the engine binds
  textures by slot (t0-t15, s0-s15) from the effect's texture list. They build for SPIR-V with register shifts (above). The
  ORG pass needs a binding model that maps the engine's slots: a fixed descriptor table per effect (16 textures, 16 samplers,
  the UAVs), b2 for raster constants and b0 for compute, and b12 (`FrameBuffer::PerFrame`, read by many for dynamic
  resolution). Converting to OrgResources later is a refactor per file once that model exists.
- **Constants.** Raster b2 comes from the engine's constant table and the custom mapper (P5.7 latches it per use); compute b0
  is the struct `SetComputeShaderConstants` uploads. `GeometryOffset` (VS c0) is (0, 0, 1/w, 1/h) of the output target;
  only FXAA's VS reads its zw.
- **Per-slot state** that the engine sets around each draw (blend, stencil NOT_EQUAL for menu 3D, viewports such as
  ISCopyCustomViewport's) is not in the shaders and comes from R2's state tables.
- **TAA_UI and TAA_Water** need sources (above). **ISMap** needs a decision.
- **Checks in game still open:** FXAA, the local map, snow (CopySubRegionCS, SnowSSS) and the lens flares with their settings
  on (the A/B covered the default settings).
