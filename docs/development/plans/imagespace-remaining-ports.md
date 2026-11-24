# Remaining image-space ports

Status: plan, 2026-11-23. Lens flares (`ISLensFlareVisibility`, `ISLensFlare`) are done on `fix/lens-flare-port`.

## Survey

The engine's SE/AE effect list (CommonLib `ImageSpaceManager.h`) against `kEffects` (`ImageSpace.cpp`) left 15 effects with
no port. Volumetric lighting (`ORGVolumetricLighting.cpp`), the screen blood splatter (DCLF) and the container effects
(GetHit, FullScreenBlur, HDR, Noise) were already covered. The 13 VR-only effects have no SE/AE slot.

Call sites came from AE Ghidra. The init (AE `FUN_141482da0`) copies most effect pointers into helper objects on the manager
(+0x168 to +0x208), which render them, so most never go through `RenderEffect`.

| Effect | Where it runs (AE) | Condition | Verdict |
|---|---|---|---|
| ISTemporalAA_UI 152 | TAA helper +0x1f0, `FUN_1414d88b0` mode 2, from `UI3DSceneManager::Render` | TAA on (CS sets it when Upscaling is TAA), scene type not 3/4/6, no `overrideBaseData` | Normal play (menu 3D scenes) |
| ISTemporalAA_Water 153 | Same function, mode not 2 | `bTAAWater:Display` (default 0) or ToggleTAAWaterBlending. Replaces ISTemporalAA every world frame | Non-default INI |
| ISSAOCameraZAndMipsCS 137, ISSAORawAOCS 145, ISSAOBlurHCS 134, ISSAOBlurVCS 135 | `Main::Draw` → `FUN_1414d71c0` (SAOCS helper +0x1d0), after the world render | `bSAO_CS_Enable:Display` (default 0) or ToggleSAOCS | Non-default INI |
| ISDownsampleIgnoreBrightestCS 122 | `RenderReflections` (AE 107146), before the ray march | `bDownSampleNormalSSR` and `iReflectionResolutionDivider:Display == 4` (default 2) | Non-default INI |
| ISDownsampleCS 121 | IBLF helper +0x180, in RenderPreTonemapComposites | IBLF on (CS forces it off in `BSImageSpace_Init_IBLF`), then `bEnableDownsampleComputeShader` (default 1) | Console only under CS (ToggleIBLF) |
| ISBasicCopy, ISDebugSnow, ISExp, ISPerlinNoise2DCS, ISReflectionsDebugSpecMask, ISDisplayDepth, ISWaterFlow | Render functions with no callers, or flags nothing reads | - | Dead; not ported |

Ghidra notes, with setting addresses: the session scratchpad `is-callsites.md`.

## Batches

Each batch is one agent in its own worktree. Agents write code only; the main session builds, tests and merges.

### A: TAA UI and water forms

CS's `ISTemporalAA.hlsl` is its own rewrite with neither form, so both need new HLSL with an ORG form. Write them from the
vanilla shaders (extracted and disassembled as in `org-full-port.md`, "Also from RE").

- **UI.** UI3DSceneManager draws the scene, then ISSimpleColor writes the mask into rt85. TAA_UI reads t0 source, t1 rt83 +
  parity (UI history), t2 rt7 motion, t3 depth, t4 rt85 mask, t5 a texture from `*(DAT_14328cc80+0x48)+0x10`. RT0 is rt41
  (`kIMAGESPACE_TEMP_COPY`), RT1 rt84 - parity. Then ISCopy into rt0. Parity does not flip in UI mode. Steps go with the
  menu steps (`MenuSteps.inl`).
- **Water.** As world TAA plus RT2 rt87 - parity; mode 1 adds t6 rt86 + parity. Replaces the world TAA step when on.
- Check: open the inventory with Upscaling on TAA (UI); `bTAAWater=1` at a river (water).

### B: SAO compute

Four dispatches in `Main::Draw` after the world render. DS is `bSAO_CS_Downscaled`; later steps use half size when set.

1. CameraZAndMipsCS: 16-byte CB, t0 depth, u0-u2 rt50-52. Dispatch ((W/2+7)>>3, (H/2+7)>>3).
2. RawAOCS: 64-byte CB, t0 rt49, t1 AO history (rt62/64), t2 rt7, u0 rt62 + DS + 2×parity. Dispatch ((W+15)>>4, (H+15)>>4).
3. BlurHCS: t0 raw AO, u0 rt66 + DS. Dispatch (ceil(W/960), H).
4. BlurVCS: t0 rt66 + DS, u0 rt46 + DS (`kSAO`). Dispatch (W, ceil(H/540)).

The composite then reads `kSAO` as with the pixel path. Check: `bSAO_CS_Enable=1`, with and without `bSAO_CS_Downscaled`.

### C: SSR and IBLF compute downsamples

- **ISDownsampleIgnoreBrightestCS** replaces the pixel form before the SSR ray march when the divider is 4. 0x130-byte CB,
  4 samples, t0 normals, t1, t2 rt7, sampler mode 4, u0 the SSR downsample target. Dispatch ((W×4+0x3ff)>>10, H).
- **ISDownsampleCS** replaces ISDownsample in the IBLF chain. Only the console reaches it, and the IBLF composites
  (`ISIBLensFlares`, `ISCompositeLensFlare*`) then also need chain steps; take them in the same batch.
- Check: `iReflectionResolutionDivider=4`; ToggleIBLF in the console.

## Risks

- All three batches add `kEffects` entries, so merges conflict in that table; merge one at a time.
- The INI-gated paths need cs-test configs that set the INI values, or the passes never run.
