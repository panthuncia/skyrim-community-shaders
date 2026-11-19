# P5.5.3: Volumetric Lighting on the render graph

Plan items 5.5.3 and 5.5.4's VL apply ([org-full-port.md](../org-full-port.md), Phase 5.5). Volumetric Lighting's GPU work runs on
ORG, so no VL dispatch or draw reaches D3D11. This deletes the census reason `imagespace: volumetric lighting compute`, and the VL
apply and VL blur never reach ImageSpace's hooks (that reason, `imagespace: effect not on the graph`, is deleted since).

Code: `src/Features/VolumetricLighting/ORGVolumetricLighting.{h,cpp}`. Switch: `CS_ORG_VOLUMETRIC_LIGHTING=1` (default off). It
does not need `CS_ORG_IMAGESPACE` or `CS_ORG_ENGINE_CONSTANTS`.

## What the engine does (Ghidra, SE 1.5.97 and AE 1.6.1170, 2026-11-11)

| Function | SE ID | AE ID | Notes |
| --- | --- | --- | --- |
| `VolumetricLightingDescriptor::Render` | 100306 | 107023 (`0x1414c4f40`) | Called by Main::Draw after the shadow casters, before the world. Copies the weather's twelve VL floats into the descriptor, rebuilds the two repartition textures when the range factor changed, makes the noise volume on its first call, then the generate and the raymarch. SE splits it: 100307-ish (`0x1412dc200`) generates, 100309 raymarches. CS's patch (VolumetricLighting::PostPostLoad) leaves one raymarch dispatch. |
| ApplyVL | 100475 | 107193 | `(helper at ImageSpaceManager+0x170, previous, output)`. One explicit-target Render of ISApplyVolumetricLighting, then `CopyResource(kMAIN_COPY depth, kMAIN depth)` on the context. |
| The VL blur | 100486 | 107206 | `(helper at +0x188, source, destination, swap)`: horizontal `source -> swap`, vertical `swap -> destination`. |
| RenderWorldPostProcess | 100430 | 107148 | `apply(previous = 43 + b, output = 44 - b)`, then `blur(output, previous, 45)`, by the descriptor's frame counter's parity. 43/44/45 are kIMAGESPACE_VOLUMETRIC_LIGHTING, _PREVIOUS and _COPY. |
| GetVLDescriptor | 100297 | 107014 | +0x00 intensity, +0x2C the repartition's range factor (the last of the twelve floats), +0x30/+0x34/+0x38 the volume's size, +0x50/+0x58 the two compute shaders, +0x60 the noise target, +0x78/+0x80 the repartition and inverse repartition textures. |
| The temporal factor | 100298 | 107015 | The apply's `g_IntensityX_TemporalY.y`, unless the helper's +0x9 flag (cleared by the apply) is set. |
| PerTechnique of the generate | 527948 | 414886 | 0x180 bytes, written by the VL render's constant helper (AE `0x1414c57a0`). |
| The noise gradient holder | 525022 | 411504 | Its object's +0x48 texture, that texture's +0x10 SRV: the apply's t4 (the sky shader binds the same). |

Samplers (shadow state modes, address then filter): the generate's s0 (0, 0), s1 (0, 1), s2 (0, 1), s3 (5, 1); the apply's every
slot (0, 1) but s4 (3, 0). The noise volume is `R8_UNORM` at the INI's `iVolumetricLightingNoiseTexture{Width,Height,Depth}:Display`,
the accumulation volume `R16_FLOAT` (AE `0x1414c5fa0`).

**The repartition textures.** Two 1D textures of 4096 `R32_FLOAT` texels, rewritten on the CPU with Map (AE `0x1414c6330`) whenever the
range factor changes, which a weather transition does every frame: texel `i` is `pow(i / 4096, k)` (the apply's) and
`pow(i / 4096, 1 / k)` (the generate's), the two function pointers at +0x40 and +0x48 (`powf`, and `1 / k` then `powf`). ORG has no
1D views, and a per-frame upload of 32 KB would need a latch of that size per frame slot. The ORG builds compute the texture's
linear, clamped sample from `k` instead (`Common/VolumetricLightingRepartition.hlsli`); hardware filtering's 8-bit fraction is the
only difference.

## How the port works

The engine's CPU side runs unchanged; none of its GPU work reaches D3D11:

1. **The VL render** is detoured. While it runs, Hooks.cpp's `DispatchCSShader` seam asks `TakeEngineDispatch` first, which withholds
   every dispatch (generate, raymarch, and the noise on the first call). After it returns, the PerTechnique block, the volume's size
   and the range factor are copied into the frame's pending work.
2. **ApplyVL** is detoured whole: its constants (intensity over depth, the temporal factor or 0) and its output are pending; the
   helper's +0x9 flag is cleared as the engine does.
3. **The VL blur** is detoured whole: its targets are checked against the apply's, and it runs the `VolumetricLighting` anchor
   (`FrameAnchors::Anchor::VolumetricLighting`, appended, one run a frame) where the engine's blur would dispatch.

The anchor's passes, each through a run gate entry the commit opens (`cs.volumetric-lighting.gate`, 13 entries):

| Pass | Shader | Reads | Writes |
| --- | --- | --- | --- |
| `noise` (once a session) | ISPerlinNoiseCS (3D) | - | `VolumetricLighting::Noise` (persistent) |
| `generate.<v>` | ISVolumetricLightingGenerateCS | the sun's cascades (`kSHADOWMAPS_ESRAM`, `kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM`, whole-array views), the noise, Terrain and Cloud Shadows | the volume `v` (transient) |
| `raymarch.<v>` | ISVolumetricLightingRaymarchCS | | the volume, in place |
| `apply.<v>.<o>` | ISApplyVolumetricLighting | kMAIN depth, the previous VL target, the volume, the noise gradient, motion vectors, kMAIN_COPY depth | the VL target `o` |
| `blur-h.<o>`, `blur-v.<o>` | ISVolumetricLightingBlurHCS / VCS | the VL target, kMAIN depth | kIMAGESPACE_VOLUMETRIC_LIGHTING_COPY, then the previous VL target |
| `depth-copy` (no gate) | | kMAIN depth | kMAIN_COPY depth |

Constants: one latch region per run (`LatchData`): the engine's PerTechnique with `1 / k` after it, the volume's size, the blurs'
VLData (`VolumetricLighting::BlurData`), the apply's values with `k`, the noise's size. Group counts are the engine's (`w >> 5`,
`h >> 5`, depth for the generate, one slice for the raymarch) and the D3D11 path's blur counts. FrameBuffer and SharedData come from
the shared constants.

**Volumes.** One per size: the exterior's and the interior's from the settings (`VolumetricLighting::VolumeSize`, as SetupVL sets
them), registered at Setup. A size outside the registration (a settings change in the menu) registers the client again and skips
frames until the graph has it.

**Dynamic resolution.** The engine's apply draws into the top-left dynamic-resolution part of the target, over which TexCoord runs
0 to 1. The ORG pass draws the whole target; the shader divides TexCoord by the ratio and discards outside, so it writes the same
pixels with the same coordinates whatever the ratio is this frame.

**Inputs.** Engine targets and depths are `FrameAnchors::EngineTarget` / `EngineDepth` imports. Terrain and Cloud Shadows are read by
their published ids (`TerrainShadows::ShadowHeight`, `CloudShadows::Occlusion`) when their ORG ports are requested, else their D3D11
views are imported. The noise gradient texture is imported (`cs.d3d11.volumetric-lighting.noise-gradient`). Samplers are the engine's
table entries, taken when the graph is built (the engine fills its table after CS's Setup). Program defines: CSHADER for the compute
shaders and the features' ImageSpace defines, as ShaderCache's D3D11 build composes them, plus CS's common defines.

## What is skipped, never handed to the engine

Each logged once per reason (`[ORG] Volumetric Lighting: a frame's volumetric lighting is skipped: <reason>`):

- the client is not in the anchor's built graph yet, or its ring is full (`FrameAnchors::CanRun`);
- an input was missing at registration (the noise gradient texture before the sky loads it): the client registers at the first
  VL render that has every input, logged as `not registered yet (...)`;
- a published input not declarable at a build (its producer has not published): its `generate` pass is left out until it is;
- a volume size outside the registration, or new engine targets (a resize): the client registers again;
- an engine sampler missing from the table at a build: the pass is left out (error);
- the apply or the blur with targets other than the VL pair (never seen; the RE says they cannot be).

A failed setup or a failed graph (`onFailed`) leaves the D3D11 path: the detours pass through. With the latch on and this switch
off, the VL dispatches and draws are no longer vouched for, so the latch skips them: run the latch with this switch on.

## Graph rebuilds

| Trigger | Handling |
| --- | --- |
| Registration | Once, at Setup (or at the first VL render when an input was missing then). |
| Interior/exterior quality differ | Both sizes are registered at Setup. No rebuild. |
| A settings change of the size, a resize | The client registers again; the graph is built at the next build point (a user action). |
| The noise volume | Persistent, made once through its gate entry; no one-shot pass. |

## Log lines

`[ORG] Volumetric Lighting: hooks installed (...)`; `on (CS_ORG_VOLUMETRIC_LIGHTING); noise volume WxHxD`; `registered with volumes
...`; `the noise volume (...) is made on the graph`; `the first frame's volumetric lighting ran on the graph`; `the VL render's
dispatch is withheld: ISPerlinNoiseCS (...)` (the first call's noise); skips as above; errors for a pass that could not be made.

## Test plan

Scenarios (`tools/cs-ab/scenarios/`): `vl-org0.env` (D3D11, latch off), `vl-org1.env` (the port, latch off) and `vl-org1-latch.env`
(the port, latch on, the census gate). Whiterun at 7:00 and 17:00 in clear weather, then Dragonsreach.

1. The log has the lines above, no `[E]` from the port, no `Render graph disabled`, no `[DCLF] Forced off`.
2. vl-org1's shots match vl-org0's: the god rays' shape and strength, their temporal smoothness, no seams at the blur's edges.
3. vl-org1-latch's census: no `77329 <- 107023` dispatch, no ISApplyVolumetricLighting draw (107733 <- 107193), no
   ISVolumetricLightingBlur dispatch (107734 <- 107209/107210 <- 107206); no `imagespace: volumetric lighting compute`.
4. With Upscaling on (a dynamic-resolution ratio below 1), the rays line up with the scene as on D3D11.

## Not verified without a run

- The engine's apply viewport is the dynamic-resolution part of the target (from Volumetric Lighting's blur comment and the
  ImageSpace port's SAO and reflections); with no upscaler the ratio is 1 and it does not matter.
- The repartition textures' format (`R32_FLOAT`, 4096 texels) and the exponent's place at +0x2C are read from the Map loop.
- The noise gradient holder is read by its RE'd offsets (+0x48, +0x10); the INI names of the noise size.
