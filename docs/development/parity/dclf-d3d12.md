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

- **Refraction** (WS4.4): done by lane par-dclf-d3d12b (below).
- **The CPU pipeline's shadow sites** (`Direct/Sites.cpp`, `Recorder.cpp`) push addresses too. They stay Vulkan-only with
  the CPU pipeline.
- **The `[DCLF] D3D12: on` line**: fixed by lane par-dclf-d3d12b.

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
- **Decoupling the opaque effects from the forward toggle**: done by lane par-dclf-d3d12b (below), without a new toggle.
  The notes that follow are the earlier analysis. The opaque effects (`Api::ObjectClass::OpaqueEffect`)
  share the `effects` toggle, which needs `forward`. The cleanest form is a toggle of their own:
  - registered with `.after = "decals"` and `.needs = { "decals" }`, which is allowed because needs resolve after every
    feature registers (`Toggles.cpp`);
  - seeded by `CS_DCLF_OPAQUE_EFFECTS`.

  Two things make that unsafe to change without a build:
  - The packed toggle word has a 32-bit budget (`kFirstRegisteredBit` plus 25 registered toggles), and one more toggle may
    not fit. Going over fails at install.
  - The effect classification (`EffectDescriptors.cpp`) and the Effect port's frame rows may assume the `effects` class is on.

  `CS_DCLF_EFFECTS` and `CS_DCLF_OPAQUE_EFFECTS` stay in `kD3D12DclfSwitches`.

# Lane par-dclf-d3d12b: refraction, effects, tree LOD and grass, setup compiles (WS4.3-4.6, WS3)

Base `org-native/integrate` 2dde906fe. Native D3D12 drew no world geometry on the test machine at the time, so none of this
is confirmed in game. Each stage below was checked offline: `dclf-shadertool d3d12` over the DXC preprocess, then DXC SM 6.6.
Every build binds only b190 and b191 (some pixel stages bind only b191).

## What changed

- **Refraction (WS4.4).**
  - On D3D12, `RefractionDrawPass` pushes the shadow layout's block words as refs and constant buffer views
    (`MainShadowLayoutViews`, `Passes/Forward.cpp`). The view block, VS_PerFrame and the zero block are refs into the main
    frame constants. SharedData and FeatureData use the main `FrameBlockViews`.
  - LodFadeFrame is pushed as a ref there, and `Utility.hlsl` reads it directly on DXIL.
  - On D3D12 the refraction signature (`shadowDrawSignature`) is Constant(`kDrawPushInstance`) then DrawIndexed, and the pass
    starts at `PlainDrawArgumentsOffset()`. `StoreShadowSequence` already wrote the instance word.
  - SPIR-V is unchanged.
- **Effects (WS4.3).**
  - The `effects` toggle no longer needs `forward`.
  - `EffectDrawOf` refuses a pass that belongs to the forward epoch while the Forward class is off: blended, Falloff unblended
    and particles. It returns `Ineligible::AlphaBlend`, as Eligibility does for blended Lighting.
  - Opaque effects and effect decals draw in the G-buffer either way.
  - The `particles` toggle now needs both `effects` and `forward`.
  - No toggle was added. The packed word holds 5 core bits and 23 registered toggles (28 of 32).
- **Tree LOD and grass (WS4.5).**
  - On D3D12, draw-family builds take `DCLF_D3D12_FRAME` (`MakeStageRequest`): DistantTree, RunGrass and tree LOD's forward
    program. FrameSource then reads their frame blocks, textures and samplers through the frame record.
  - Checked: colour, depth, TruePBR and forward permutations, without feature defines and with these: SSS, IBL, Dynamic
    Cubemaps, height fog, cloud and terrain shadows, Skylighting, volumetric shadows, LLF, Grass Collision, Grass Lighting,
    wetness, HDR, linear lighting.
  - Grass reads its visible entry by `SV_InstanceID` counting from the first instance. It draws with a new family signature,
    `Api::DrawFamilySignature::InstanceWordDrawInstanced`:
    - A 20-byte record: the first instance word, then DrawInstanced's arguments (`GrassLayout.h` `DrawArgs`, written by
      `GrassArgsCS`).
    - D3D12's signature writes the word into `kDrawPushInstance`, and `RunGrass.hlsl` adds it.
    - Vulkan's signature starts 4 bytes in (`FamilyPipelines::argumentsOffset`).
    - The draws buffer grew from 221,248 to 270,400 bytes.
  - Tree LOD keeps the plain signature: every draw has first instance 0, and the stage adds phase two's start itself.
- **Setup compiles (WS3).**
  - `LoadProgram` (`Resources/Helpers.h`) loads a setup's compute programs on the build executor and caches them per device.
  - Before it makes anything, the main setup asks for every BuildDraws entry, the forward sort and the sort scatter.
  - While any program is loading, the main setup returns not ready and logs `[DCLF] Main-pass draws not ready: its compute
    programs are compiling off the render thread`. DCLF stays off, the engine draws, and the setup runs again next frame.
  - The shadow setup (`shadow views not ready: the compute programs are compiling ...`) and the view setups wait the same way,
    without failing.
  - `PrefixSum::Load` and the sort scatter's own load in `DrawSort::Create` still run on the render thread. They are small,
    and by then the scatter's compile is cached.
- **Log line.** It now reads `[DCLF] D3D12: on, the GPU pipeline with plain indirect draws (every part but the D3D12 switch
  table's)`.

Rows removed from the switch table, each in its own commit:

- `CS_DCLF_REFRACTION`
- `CS_DCLF_EFFECTS`, `CS_DCLF_OPAQUE_EFFECTS`, `CS_DCLF_BILLBOARDS`
- `CS_DCLF_GRASS`, `CS_DCLF_LOD_TREES`

`CS_DCLF_FORWARD` is still 0 on D3D12. Refraction and the blended effects therefore draw nothing there yet. Opaque effects,
effect decals, grass and tree LOD do draw.

## How to test

- **Native D3D12** (`native-d3d12.env`), once the world draws there.
  - Log: the off list must not name the removed rows. There must be no `the D3D12 build's frame rewrite failed` and no
    `D3D12 refraction: no buffer view ref`. `grass pipelines ready` and `tree LOD pipelines ready` should appear.
  - Shots: compare with native Vulkan. Forge coals and other opaque effects in Whiterun, grass and distant trees.
  - With `CS_DCLF_FORWARD=1`, also check heat haze over braziers (refraction) and blended effects.
- **A failing build:** run with `CS_D3D12_SOURCE_DUMP=<dir>` and feed the dumped source to `dclf-shadertool d3d12`.
- **Guest and native Vulkan:** grass changed layout on both backends (20-byte draws, signature 4 bytes in). Check that grass
  draws, in both depth phases, and moves in the wind.
- **Watchdog:** change `BuildDrawsCS.hlsl` (a comment), then launch guest and native. The log should show the not-ready
  line, then DCLF comes on a few seconds later with no hang report.
- **Forward off on Vulkan** (`CS_DCLF_FORWARD=0`): opaque effects stay DCLF's, and blended effects are the engine's.

## What is left

- **Water reflections (WS4.6) stay in the table.**
  - The faces would build. Their pipelines are pulled builds with the frame rewrite now, and their draws use
    `zDrawSignature` and the view's frame push words.
  - Reading the code turned up a problem on both backends, in how each face gets its camera:
    - `ViewEpoch.cpp` writes each face's VS and PS b12 (VS_PerFrame) into that face's frame push words.
    - Since the frame block views (a992281e1, 2026-10-27), the main and Z layouts read every pass-wide block through the frame
      record's `frameBlocks` view, b12 included. The D3D12 rewrite does the same (`DCLFFrameBlockIndex`).
    - A view has one record and one set of views, over its own frame constants. Its commit never writes their b12 slot.
  - The effect: all six faces would read one unwritten VS_PerFrame. First person uses the main frame constants and pushes
    its own b12, so it would draw with the main camera's projection.
  - To confirm: check the Reflection feature's face-camera parity line and a first-person shot on Vulkan.
  - The fix: a record per face (or per-face b12 views), with the record ref pushed per face.
- **Shader reload device loss on D3D12** (`dclf:reload-shaders`) is not fixed.
  - It is not a missing frame rewrite: DCLF's rebuild path takes it (`CompileProgram` -> `CompileStage`).
  - The cause is probably outside DCLF's pipelines: the retirement of replaced pipelines, or kept recordings that still bind
    them (RenderGraphRuntime, ComputeProgram).
  - Next step: run it once with the D3D12 debug layer.
- These rows also stay in the table: particles and master particles, first person, and the CPU pipeline rows.
