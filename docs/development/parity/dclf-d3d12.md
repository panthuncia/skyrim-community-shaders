# DCLF on D3D12: family vertex stages and shadow views (parity wave, lane par-dclf-d3d12)

Plan: WS4.1 and WS4.2 of the native parity plan. Base `org-native/integrate` f34fce25d. Background:
`org-full-port.md`, "DCLF on D3D12: the port's plan" and "Slice 1".

## What changed

### WS4.1: the engine-draw families' vertex stages pull on D3D12

These vertex stages had a branch for non-SPIR-V builds that compiled a stage drawing nothing:

- `Water.hlsl`, `Sky.hlsl`, `Particle.hlsl`, `BloodSplatter.hlsl`
- `Utility.hlsl`: the shadow mask build and the menu scene build
- `Effect.hlsl` (`EFFECT_ORG`), `Lighting.hlsl` (`LIGHTING_ORG`)
- `RenderGraph/DecalOcclusion.hlsl`
- `ISWaterDisplacement.hlsl`: the ripple makers

The guards dated from the time these stages loaded through `vk::RawBufferLoad`. They now read through buffer view refs
(`DeviceLoad`, `Common/DeviceRef.hlsli`), which work on DXIL too, so the guards are gone.

Nothing else needed to change:

- The family draws are `RunGate::OpenDraw` draws with the first vertex and the first instance at 0. The stages read neither
  `SV_InstanceID` nor a first vertex, so D3D12's instance rule does not matter here.
- The stages bind only the ORG push block (`OrgBindings`, b0, root constants), the same as every other ORG program on D3D12.
- They do not use `VS_PerFrame` (the b12 space-1 rule applies only to the DCLF builds).

Checks:

- DXIL builds with SM 6.6, `-HV 2018`:
  - Water: SIMPLE; SPECULAR NORMAL_TEXCOORD, with and without VC
  - Sky: TEX, HORIZFADE, OCCLUSION, DITHER TEX
  - Particle: plain and ENVCUBE
  - Utility: the shadow masks, base texture, depth, skinned shadow map, fog of war
  - Effect and Lighting ORG builds, several permutations each
  - DecalOcclusion
  - both ISWaterDisplacement ripple makers
- Each DXIL build binds only cb0.
- The SPIR-V of Lighting, Utility, Water and Effect is byte-identical to before.

### WS4.2: the shadow views and the occlusion maps on D3D12

The Skylighting and precipitation occlusion maps draw through the same `ShadowViewPass`, so they come with the shadow views.

- **Draw records** (`GpuLayouts.h`, `kShadowDrawBytes`): 24 bytes on both backends. The first word is the draw's first
  instance, followed by DrawIndexed's arguments at `kShadowDrawArgumentsOffset` (4). The slot unit is now 36 bytes
  (`kShadowDrawUnitBytes`). `BuildDrawsCS`'s `StoreShadowDraw` writes the instance word.
  - On D3D12, `shadowViewDrawSignature` is Constant(`kDrawPushInstance`) followed by DrawIndexed, read from the record's start.
    The pulled stage already adds `DCLFInstanceWord` in `DCLFDrawSequence`.
  - On Vulkan, the signature is plain DrawIndexed with a 24-byte stride, and `ShadowViewPass` starts it 4 bytes in.
  - One layout serves both backends. The cost on Vulkan is 4 more bytes per draw record, with no backend branch in the
    compute shader.
- **Push words** (`ShadowEpochPasses.cpp`, `ShadowViewPass::Record`). On D3D12, the view's words 4-13 are replaced at record
  time:
  - the view block's ref and its VS_PerFrame copy's ref, from the recording's view-blocks version;
  - the zero block's ref, in the shadow constants;
  - the constant buffer views of SharedData and FeatureData.

  The views (`ShadowResources::blockViews`) are made with the shadow constants, on D3D12 only
  (`FrameBlockViews::CreateFor`, a generalisation of the main pass's frame block views). SharedData and FeatureData are
  structs without packoffset, so only a constant buffer view gives their cbuffer packing. Vulkan pushes the shape's
  addresses as before. Words 0-3 (the record address and its ref) are unchanged.
- **Frame rewrite**. `ShaderPrograms` adds `DCLF_D3D12_FRAME` and `DCLF_D3D12_SHADOW_FRAME` to pulled Utility builds on
  D3D12. `DxilStages::PrepareFrameSource` then runs `FrameSource::Rewrite` with `Options::shadow`:
  - Textures and samplers come from the shadow frame record, as in the main builds.
  - b5 and b6 are read through `DCLFShadowBlockView`. Every other block is read register by register through
    `DCLFShadowBlockRef`: b0 is the view block, b12 is VS_PerFrame, and anything else is the zero block. This is the same
    mapping as `ShadowLayoutRanges` on Vulkan.
  - A VS b1 declaration is refused, because the pulled stage reads its material row itself.
  - `Common/DCLFFrameRecord.hlsli` declares the 14 push words under that define, ending at word 13, so the block fits the
    layout's root constants.
- **Shader checks** (`dclf-shadertool d3d12 ... --shadow`, which is new). These permutations build to DXIL binding only b190
  and b191, in both VS and PS:
  - RENDER_SHADOWMAP canonical: SKINNED; with TEXTURE VC ALPHA_TEST; TREE_ANIM; VC NORMALS TREE_ANIM
  - RENDER_SHADOWMAP_PB, with and without the alpha test
  - LOD_LANDSCAPE and FOCUS_SHADOW
  - OPAQUE_EFFECT GRAYSCALE_TO_ALPHA
  - RENDER_DEPTH: plain, alpha-tested, skinned, LOD_LANDSCAPE
  - STENCIL_ABOVE_WATER and DEPTH_WRITE_DECALS
  - the refraction build (RENDER_NORMAL)

  The push block is 56 bytes. `BuildDrawsCS` (ShadowViews, ShadowCasterRows) builds for both DXIL and SPIR-V.
- **Switches**: `CS_DCLF_SHADOWS`, `CS_DCLF_SKYLIGHT` and `CS_DCLF_PRECIPITATION` are gone from `kD3D12DclfSwitches`.

## How to test

- **Scenarios:** `tools/cs-ab/scenarios/native-d3d12.env`, or cs-test `native-d3d12` (`-quick`, `-tour`). Add
  `native-d3d12-validation.env` for a D3D12 debug-layer run. Run guest and native Vulkan once as well, because the draw
  record changed layout there too.
- **Log**:
  - `[DCLF] D3D12: on, ...; still off there: ...` must no longer list `CS_DCLF_SHADOWS`, `CS_DCLF_SKYLIGHT` or
    `CS_DCLF_PRECIPITATION`.
  - `[DCLF] shadow views recorded: N views, M indirect draws` should appear.
  - No `[DCLF] D3D12 shadow views: no buffer view ref ...` line.
  - No `shadow block views` error.
  - No `the D3D12 build's frame rewrite failed` or `shadow pipeline ... failed` lines.
- **Shots** (`dclf:shot` at Whiterun and Dragonsreach):
  - Sun shadows on terrain, buildings and NPCs: cascades with no gaps, no acne from a wrong depth range, and alpha-tested
    foliage shadows with holes.
  - Interior point-light shadows: the paraboloid hemispheres.
  - Skylighting, which reads the occlusion map: no flat or black sky occlusion.
  - Rain occlusion under roofs: rain must not fall indoors under overhangs.
  - Compare each shot with native Vulkan.
- **Validation layer**: the shadow constants are declared as a ShaderResource and read as constant buffer views (b5, b6).
  The main pass's frame block views do the same, so a state warning there would be shared with slice 1.

## What is left

- **Refraction** (WS4.4) stays off. `ForwardPasses`/`MainShadowLayoutPush` push block addresses. On D3D12 they need the
  same refs and views. The refraction signature (`shadowDrawSignature`) needs Constant+DrawIndexed and
  `PlainDrawArgumentsOffset()`. Its stages already build with the shadow rewrite.
- **The CPU pipeline's shadow sites** (`Direct/Sites.cpp`, `Recorder.cpp`) push addresses too. They stay Vulkan-only with
  the CPU pipeline.
- **The `[DCLF] D3D12: on` line** (`DrawcallLimitFix.cpp`) still says only the main view's Lighting draws. The off list it
  prints is correct.

## WS4.3 (Effect family): findings so far, no code change

- **Offline checks.** The Effect pulled stages were checked with `dclf-shadertool rewrite`, then the preprocess, then
  `dclf-shadertool d3d12`, and DXC SM 6.6. The rewrite used the game's options: material textures 0-13, 15, 71, 74, 80-91;
  shadow mask 14; 16 samplers; eager and lazy rows. Every permutation tried passes the frame rewrite and builds to DXIL
  binding only b190 and b191:
  - VC TEXCOORD TEXTURE, skinned, MEMBRANE, SOFT PROJECTED_UV, LIGHTING, GRAYSCALE_TO_ALPHA and GRAYSCALE_TO_COLOR,
    ALPHA_TEST, TEXCOORD_INDEX, PARTICLES, STRIP_PARTICLES, BLOOD, MULTBLEND_DECAL, SKY_OBJECT, MOTIONVECTORS_NORMALS
  - DCLF_DEPTH_ONLY
  - with and without the feature defines (Light Limit Fix, Skylighting, IBL, cloud shadows, height fog, volumetric shadows,
    wetness, dynamic cubemaps)

  The rejections the plan names (`FrameSource.cpp` resource arrays and member types) did not reproduce. A game run with
  `CS_DCLF_EFFECTS=1 CS_DCLF_FORWARD=1` on D3D12 should look for `the D3D12 build's frame rewrite failed` lines. If one
  appears, run `CS_D3D12_SOURCE_DUMP=<dir>` and feed the dumped source to `dclf-shadertool d3d12`.
- **Decoupling the opaque effects from the forward toggle** is not done. The opaque effects (`Api::ObjectClass::OpaqueEffect`)
  share the `effects` toggle, which needs `forward`. The cleanest form is a toggle of their own:
  - registered with `.after = "decals"` and `.needs = { "decals" }`, which is allowed because needs resolve after every
    feature registers (`Toggles.cpp`);
  - seeded by `CS_DCLF_OPAQUE_EFFECTS`.

  Two things make that unsafe to change without a build:
  - The packed toggle word has a 32-bit budget (`kFirstRegisteredBit` plus 25 registered toggles), and one more toggle may
    not fit. Going over fails at install.
  - The effect classification (`EffectDescriptors.cpp`) and the Effect port's frame rows may assume the `effects` class is on.

  `CS_DCLF_EFFECTS` and `CS_DCLF_OPAQUE_EFFECTS` stay in `kD3D12DclfSwitches`.
