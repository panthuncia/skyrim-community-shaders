# snap3-engstate: native records with no engine-bound state (phase 3)

Lead: `engstate`, branch `snap3/engstate` (base `org_integration` 86d4e3cd9), stage `s3-engstate`. Phase 3 of
[render-thread-snapshots.md](render-thread-snapshots.md); decisions 8, 41-43, 57, 109-116 of
[render-thread-decisions.md](render-thread-decisions.md); follows `snap3-resolve.md` (item 2's producer list) and `snap3-llfhang.md`
(the viewport defect; the side branch `snap3/llfhang-viewport` was read, not merged: it mapped the engine's viewport by a size ratio
inside the resize window, which this branch replaces with a viewport computed from the revision).

| Item | State |
|---|---|
| 1. The viewport from the revision (decision 112) | done: TAA resize stress passes |
| 2. CS producers for StartAccumulating and the post-resolve; stop calling them | not done (producers for the depth shift, the eye, the per-frame block's users; the calls remain) |
| 3. Remaining engine-state readers | partly: Forward's FrameParams.z, DCLF's constant buffers, eye, cull camera, viewport and depth range, the sky's references and eye, the view epochs' block, the shadow views' diagnostic; left: DCLF's bound views and targets, the water capture's globals, the ImageSpace copy rows |

## 1. Viewports (decision 112)

Rule (Renderer::UpdateViewPort, AE 0x140e441c0): the viewport is the camera's port times the size of colour target 0 (else the depth
target) as the target manager recorded it, times the dynamic resolution ratios. Native now takes the size from the **adopted
revision's** `EngineTargetShapes`, the targets the frame really draws into, never the engine's live viewport.

- `Family::FrameViewport(colour, depth, bias, shifted)` (EngineDrawFamily): adopted shape x `FrameCamera::ViewportRatios()` (the
  render frame's ratios; 1 after the upscaler). `LatchViewport` inside a native RenderWorld point (WorldPass) uses it with the point's
  targets: sky main batch and sun, water mask and surface.
- Shadow mask (world view, native): `FrameViewport(kSHADOW_MASK)`, and its texel scale (`vposX/Y`) from the adopted shape.
- Precipitation (native): `FrameViewport(kMAIN, kMAIN depth)`.
- DCLF's main shapes: the size is no longer a capture of the engine's viewport. `PostRevisionInputs` computes it from the revision the
  recordings name: `Frame::Revision::FragmentVersions` now carries that revision's variables (the preparing revision's, else the
  adopted ones), and the size is kMAIN's shape there times that revision's `DynamicRatios`. Viewport and frame versions come from one
  call, so a fragment's render area always fits the versions it is adopted with (the earlier attempts failed because the engine's
  viewport and targets change on different frames).
- Blood splatter already used the adopted display size (snap3-world).
- Menu scenes, decal occlusion and reflection faces keep the engine's (not native world frame; faces set their own rect).

Observe phase (`s3-engstate-a-observe-up`, native-upscalers, all four upscalers): outside a resize the computed viewport equalled the
engine's at every native world site (shadow mask, sky, sun, water: thousands of calls, 0 differences; DCLF main 1,261 per epoch, 1
difference at an upscaler switch, where the revision's ratio follows one frame later, as intended). Menu scene shadows use a custom
port (4096x2048 of 4096x4096): left on the engine's, as planned.

## 3. Readers converted

- **Forward's FrameParams.z** is a constant 1 in both modes: the post-resolve (AE 106584) writes the per-frame block with flag 4
  (AE 0x140e45680: z = flag 4) right before both blended draw calls. FrameCamera's 0 is right for the deferred part. The value guest
  read was always 1, so guest is unchanged; the bound read and `NoteBlendedFrameParams` are gone.
- **DCLF `CaptureBindings` (native):** no bound constant buffer is read (observed: only VS b12, PS b5, b6, b12 are bound there, all
  supplied by `SupplyFixedBlocks`: the frame camera and CS's state). The eye and previous eye are the frame camera's (observed equal;
  the previous one is now the last drawn camera, matching decision 43's rows). The cull camera is the frame camera's unjittered
  view-projection (what the mirror of the engine's block held in native). Viewport size: above; depth range: below. Still read:
  the bound pixel views (t65 caustics, t73 hair tangent shift, t78/t79 static IBL: CS features' own textures, read through the bound
  context) and the bound targets and depth.
- **Depth range (the LOD depth shift):** inside the resolve functions' windows (AE 107125 to 107126) the engine lowers the depth
  range's maximum to a constant (304859 / 254902, 0.999998) while ToggleDepthShift is on. Native's `BlockId::RenderWorld`
  (`WorldPass::Block`, captured on the game thread: the toggle, the scene graph, the map flags, the constant) and `WorldPass::State::shifted`
  give `WorldPass::DepthRange`. Water mask and surface and DCLF's main pass are inside a window, sky and sun outside. Observed: the sky,
  sun and water ranges matched the engine's at every call; DCLF's colour capture matched (the Z-prepass capture reads 1.0 but the
  Z epoch takes the main pass's range, as before).
- **Sky `Frame::Read` at native points:** the alpha reference (128/255, set by the pre-resolve) and stencil reference from WorldPass,
  the eye from the frame camera (observed equal to the accumulator's over 2,000+ reads); no shadow state read there.
- **View epochs' block (`FrameBlockNow`):** native takes the frame camera's block; its FrameParams and ratios were what the engine's
  held for first person (0 differences), and the faces' FrameParams matched (their ratios are written after, as before).
- **Shadow views:** the per-view diagnostic no longer reads the engine's block for views DCLF draws with its own camera (6.85 bound
  reads per frame, all diagnostic).
- **Main view:** the development-build bound-format check runs in guest only.

## Detector (native Vulkan, bound-state reads per world frame)

The total moves with the scene (latched families, ImageSpace), so it is read per site. Removed from native's world frame: ShadowCapture
6.85, ViewEpoch `FrameBlockNow` 2.0, ForwardHooks 1.0, MainViewPoints 1.0 (10.85 per frame; 21.4 -> about 10.6 in the same
native-upscalers scene). Left (`-d-17`, quick): SharedConstants (Start/PreDepth/Ui segments, other cameras), Deferred's two CS b12
binds (1.0 each), CaptureBindings' marker 2.0 (views and targets still read), FrameRecords' first-person frame buffer 0.7, the engine
constants capture and ResolveHeader of the latched families (scene-dependent). Engine render calls per world frame: unchanged (56,
quick), as no call was removed.

## 2. Not done: StartAccumulating and the post-resolve

Native still calls them. What replacing them needs, in call order (AE 106583 / 106584 decompiled; SE 99938 / 99939):

- Pre-resolve: depth mode 4; depth shift on; RenderBatches #1 (+0x84: Deferred's `Main_RenderWorld_Start`: StartDeferred, DCLF
  BeforeOpaquePass with SetDirtyStates, whose flush triggers the G-buffer clears); empty groups; depth mode 3; shift off; alpha test
  128/255; sky batch (+0x237); cloud group (+0x274); shift on; blended decals; opaque decals (+0x308: AfterOpaquePass, terrain
  blending, EndDeferred, DeferredPasses, composite, depth copy); water mask check, stencil clear, mask targets and batch (+0x492),
  ImageSpace+0x1f0 +0x2c = 1; with flag 0x40: targets cleared, ApplyState (skeleton clears), RenderReflections (+0x55B, its own
  SetCameraData), kMAIN and main depth bound, depth mode 1; shift off.
- Post-resolve: shift on; underwater blended draw; SSR test (ImageSpace+0x1f0, water height, flags); group 10 and water batches
  (+0x143, +0x161); water blend (+0x1BE); SetCameraData (+0x239 PostWater_SetCamera); groups 11, 12; per-frame block with flag 4;
  group 7; per-frame block; blended draw (+0x543) with flag 4; per-frame block; last batch; shift off.

Producers in place now: the depth shift (block), the per-frame block's consumers in native (FrameCamera everywhere DCLF reads it),
Forward's z. Still needed: each thunk split into its CS part and the engine call, the G-buffer and stencil clears as CS clears (the
skeleton's), the shadow-state target writes RenderPostProcess and DrawSun's SetCameraData successors read, and the render-state reads
of later code (ShadowMask's `rss.posAdjust`, the reflections). Decided by the agent, to confirm: left for its own item, since a partial
copy of the bodies would still call the same engine functions.

CS direct calls to engine render functions from RenderWorld's scope (unchanged by this branch, decision 57 hides them): StartAccumulating
(SetCameraData, the accumulator's setup, the pre-resolve with 21 sites), the post-resolve (20 sites), SetRenderTarget x4-6, the
ImageSpace copy or RenderSnow, RunReflections' SetCameraData/SetRenderTarget/SetDepthStencil, RunSsao's and RunWaterBlend's
SetRenderTarget/SetDepthStencil, DrawSun's SetCameraData, BeforeOpaquePass's SetDirtyStates.

## Runs (stage `s3-engstate`, Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-upscalers, observe, detector | `s3-engstate-a-observe-up` | passed; parity numbers above |
| native-vulkan+window-resize-stress-taa (first switch build) | `-b-wrs-taa1` | passed (12 resizes; before: device lost at the first grow) |
| native-vulkan+rain, detector, observe | `-c-rain`, `-c-rain2` | passed; depth range rule confirmed |
| native-vulkan+quick, observe | `-c-quick2` | passed |
| native-vulkan+window-resize-stress-taa x3 | `-d-1`, `-d-2`, `-d-3` | all passed |
| native-vulkan+window-resize-stress (FSR) | `-d-4` | passed |
| native-vulkan+window-resize, resize-drag | `-d-5`, `-d-6` | passed |
| native-upscalers | `-d-7` | passed |
| native-vulkan+cadence-drop, -drop-cells, -repeat | `-d-8..10` | passed |
| native-vulkan+shadows-blend | `-d-11` | passed |
| native-interior, native-river | `-d-12`, `-d-13` | passed |
| native-vulkan+post-effects | `-d-14` | passed |
| native-tour | `-d-15` | passed |
| guest+quick | `-d-16` | passed |
| native-vulkan+quick, detector | `-d-17` | passed (56 render calls) |
| native-d3d12-quick | `-d-18` | passed |

SE was not run (new addresses are SE/AE pairs from the SE disassembly of 100407).

## Decided by the agent, to confirm

- ES-a. Native viewports come from the adopted revision's target shapes at the render frame's ratios; DCLF's main shapes from the
  revision their recordings name, at that revision's `DynamicRatios` (a ratio change reaches DCLF one revision later, as a capture did).
- ES-b. Forward's FrameParams.z is the constant 1 in both modes (the engine's value at both blended draws).
- ES-c. The depth shift is captured on the game thread as `BlockId::RenderWorld`; DCLF's main pass and the water points use the shifted
  range, the Z-prepass the main pass's (unchanged rule).
- ES-d. Native DCLF previous eye is the frame camera's last drawn one (was the engine's previous update); identical at normal cadence.
- ES-e. `window-resize-stress-taa.toml` is added (god mode first, TAA, six resizes).

## Open items

- Item 2 above.
- DCLF's bound views (CS features' own textures: give them frame texture registrations) and bound targets in native.
- Water `Port::CaptureFrame`'s engine globals (render-half writes: render size, jitter flag, effect camera, SSR state) and the ImageSpace
  copy step's rows.
- Menu scenes still latch the engine's viewport (phase 5).
