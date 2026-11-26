# Snapshot roadmap phase 5: ImageSpace from game data (lead `imagespace`)

Branch `snap5/imagespace` (base `org_integration` b67fb9e4c), stage `s5-imagespace`. Roadmap: [render-thread-snapshots.md](render-thread-snapshots.md),
phase 5. Decisions 8 and 41-43 in [render-thread-decisions.md](render-thread-decisions.md). Background:
[engine/imagespace.md](../engine/imagespace.md), and the removed plan `git show 13403653f:docs/development/plans/dclf-native-imagespace.md`.

## Where it started

The world chain already drew natively (`NativePost.inl`, `NativeWorld.inl`): the containers (HDR, end-of-frame, composites,
RenderWorldPostProcess's body, the reflections pass, RenderSSAO, the water blend) are not called, and CS computes every step's
rows with the engine's formulas. But it computed them from engine memory read at render time: the ImageSpace manager's blended
data, INI globals, the effects' members and the current camera, at Renderer::Begin (read point A), at RenderWorldPostProcess's
entry (B) and at each container's hook. Menu chains, the frozen-background composite and DrawFrame's PrintScreen copies still go
through the engine's `RenderEffect`.

## What was built

### The ImageSpace block (`src/RenderGraph/Engine/ImageSpace/ImageSpaceBlock.h`)

`BlockId::ImageSpace`, captured by `NativePost::CaptureBlock` (registered at `ImageSpace::Install`, graph modes only):

- `GameInputs`: the blended ImageSpace data (manager +0xC8..+0x15C), the HDR, adaptation and TAA settings (world map forms
  picked), the bloom blur effects' radius and bright flag, the chain's switches (HDR, EOF, usesLDR, SIL, IBLF), the pause flag,
  the frame time, the slots the game turns on (every slot's `IsActive` but refraction's), a save's screenshot request, the frame
  counter, and the slot effects' inputs (refraction tint, the depth of field's INI and underwater data, the radial blur's shader,
  the blur effects, get hit, the timer).
- `WorldSettings`: the reflections pass's and RenderSSAO's INI globals, the snow sparkles, the composite's two SSR values, the sun
  shadow light's direction and the sun light's diffuse.
- `adaptResets`: a count (below).

One reader per part (`ReadGameA`, `ReadGameB`, `ReadWorldSettings`) serves both producers: native calls them in the capture;
guest calls them at A, B and the container hooks, where it read before. `ImageSpaceFrame` derives from `GameInputs`, so the
formulas did not change. Native picks the block in `NativeBlock()` (null in guest and before the first adoption).

Native also reads the world camera from the snapshot's main camera (`MainCamera()`, fatal if a world frame has none): TAA's
near/far, RenderSSAO's frustum and near/far, the composite's near/far and eye.

### Observe, then switch

A temporary observe build (commit dae15927d, deleted in 50a72922f) compared, in native, every block field with the engine read at
the old read point, plus render-time camera and sun candidates against the snapshot's main camera and `CoreFrame`:

| Run | Frames | Differences |
|---|---|---|
| `s5-imagespace-obs-transitions` (native-vulkan+imagespace-transitions: Whiterun day, Dragonsreach, back) | 3037 at A, 2136 at B | every A, B and W field 0, except `volumetric` (2, one frame at each cell change) and `menu` (1, the exit frame); the world camera at B, the current camera at RenderSSAO and at the composite, and the sun direction: 0. The current camera at the water blend: far plane differs every frame (371648 vs 353840). The current camera at B and the depth of field's camera are other cameras (near 1, far 10000 / 2). |
| `s5-imagespace-obs-sky` (sky-native: noon, cloudy, timescale 20) | 1388 world frames before the load, 673 at B | same; `data` differs only in loading frames (the loading menu's own ImageSpace on the serving thread, no new snapshot there; no world chain runs) |

So `volumetric` (VL Render sets it during the frame) and the water blend's camera stayed render reads; everything else switched.

The observe run missed one thing: the adaptation reset is consumed, not read. The first switched build reset at A, and lost the
reset when the frame that saw the new count ran no HDR effect (loading frames): the quick shot came out dark (`sw-nvk-quick`,
mean 18 vs 65). Now the count is applied by the first HDR effect after it moves (`RunHdr`); `sw2-nvk-quick` matches the base
(mean 64.6 vs 64.8, the rest is the guard's pose).

### PreRender/PostRender and the containers' side effects

The containers' writes CS keeps (native), by who reads them:

- **Game-visible, now on the game thread:** the adaptation reset (415053; Sky::Update sets it, the HDR effect cleared it). The
  capture takes it in a native world snapshot (unless another mod keeps the HDR effect on the engine) and bumps a count; the
  render side never writes the flag in native.
- **Game-visible, still written at render:** the save screenshot handshake (manager +0x211 cleared, +0x218 the frame; S15). The
  request is now captured, so a request made inside DrawFrame (+0x5DA, after the capture) is copied one update later. Making the
  completion a snapshot event result belongs with S15's port.
- **Read only by the engine's render code** (the engine path's frames, the menu scenes' HDR and TAA, later containers): the
  luminance parity and adapted target, TAA's object fields and `taaReset`, refraction's drawn flags, the underwater mask flag,
  get hit's blur flag, the slot count, the shadow-state targets, viewports and blend modes, the current camera, the depth of
  field's near/far, the SSR camera and flags, the SAO counter, the water blend's clear request. They go when the engine's
  render half and the menu chains stop running (phases 3 and 5's menu scenes).
- Shader `PreRender`/`PostRender`: the native world chain does not call them (unchanged); only the menu steps' native path does.

### The engine-render detector

`EngineZones::CallTarget(site)`: the function behind a counted call site, past the detector's thunk. RunBody calls the
end-of-frame effects through their call site (so Upscaling's, HDR Display's and other hooks there run in order) with it, so the
detector no longer counts CS's own call as the engine entering `RenderEndOfFrameEffects`. `ShaderOf` keeps each effect object once
found (the manager keeps them for the session): ImageSpace.cpp's largest record-time site (48.6 reads per world frame) goes.

ImageSpace entry points the engine still enters in a native world frame: `RenderSSAO` x2, `RenderReflections`, the water blend
and the snow copy (`DispatchCopySubRegion`) from RenderWorld/RenderPreResolve/RenderPostResolve, and `RenderPostProcess` from
RenderPlayerView. CS's detours replace their bodies; the calls go when phase 3 stops running the engine's render half.

## Commits (branch `snap5/imagespace`)

- dae15927d `feat(imagespace): snapshot block, observe parity` (the observe build)
- 50a72922f `feat(imagespace): native reads the snapshot block` (the switch; observe deleted)
- 80eb5a9f2 `fix(imagespace): description from the snapshot` (the frame description and the end-of-frame check read the
  same block: turning on Post Processing's depth of field flipped the vanilla one's `IsActive` between the capture and Begin, a
  fatal tap error in `post-effects` on 50a72922f; plus the `ShaderOf` cache)
- 0d092a714 `feat(detect): CS's calls through a site not counted`
- docs: this file, the roadmap's table row

## Runs (stage `s5-imagespace`, Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-vulkan+imagespace-transitions | s5-imagespace-obs-transitions | passed (observe) |
| sky-native | s5-imagespace-obs-sky | passed (observe); stuck on the loading screen after `coc Riverwood` with timescale 0, see Left |
| native-vulkan+imagespace-transitions | s5-imagespace-sw-transitions, -sw2-transitions | passed |
| native-vulkan+quick | s5-imagespace-sw-nvk-quick | passed, dark shot (reset lost, fixed before the commit) |
| native-vulkan+quick | s5-imagespace-sw2-nvk-quick | passed, matches the base |
| guest+quick, native-d3d12-quick | s5-imagespace-sw2-guest-quick, -sw2-d3d12-quick | passed (dark shots: detector runs, below) |
| native-interior | s5-imagespace-sw2-interior | passed |
| native-vulkan+post-effects | s5-imagespace-eof-post-effects | hang: the fatal tap error fixed in 80eb5a9f2 |
| native-vulkan+quick, +menu, +ui-menus (detector) | s5-imagespace-eof-* | crashed: a recursion in my first `Entries()`, fixed before 0d092a714 |
| native-vulkan+post-effects | s5-imagespace-v3-post-effects | passed |
| native-vulkan+quick (detector) | s5-imagespace-v3-nvk-detect | passed; `RenderEndOfFrameEffects (in RenderPostProcess)` no longer counted, `ShaderOf` site gone |
| native-vulkan+menu | s5-imagespace-v3-menu | passed |
| native-vulkan+ui-menus | s5-imagespace-v3-ui-menus | crashed at exit after "[TEST] outcome passed" (status not written); passed on the rerun below |
| native-vulkan+cadence-repeat | s5-imagespace-final-cadence-repeat | passed; against the base's cadence-repeat run, mean channel difference 19.0 / 19.7 / 32.5, the floor between two base runs 16.6 / 16.0 / 31.6 |
| native-vulkan+ui-menus | s5-imagespace-final-ui-menus | passed |
| guest+quick, native-d3d12-quick, native-vulkan+quick | s5-imagespace-final-* | passed; shot means 65.1 / 65.2 / 65.3, base 64.9 / 64.9 / 64.8 |

**Dark shots with `CS_SNAPSHOT_DETECT=1`.** Every quick run with the detector on, from any lead, shows a dark start shot (mean
about 18 vs 65): `s2-camera-v3-detect`, `s3-lights-f-nvk-quick`, `s3-lights-f-d3d12-quick` and mine. Runs without it are
normal. Compare shots only from runs without the detector.

## Decisions by the agent, to confirm

- The block is ImageSpace's own (`BlockId::ImageSpace`), not part of Post Processing's: the native chain needs it with Post
  Processing not loaded, and it needs the manager's raw data (Post Processing's block swaps in the override base data).
- The render target sizes, the normals and history parities and the dynamic-resolution ratios stay render state (CS's targets,
  Upscaling's ratios), not snapshot data.
- Fields that the render half writes stay render reads until phase 3 replaces the writer: refraction's activity, VL's activity
  and colour, the depth of field's steps and camera, the full-screen blur's camera test, the water blend's camera and posAdjust,
  the portal fog, `saoReflections`, the SAO helper's sparkle colours.
- A world frame in native without a snapshot main camera is fatal (`MainCamera`), no fallback to the engine's camera.
- The vanilla depth of field's `IsActive` includes Post Processing's skip flag (CS state). It is captured with the other slots on
  the game thread, so the flag reaches the frame through the snapshot like the slots do.

## Left

- **Menu chains**: the menu scenes' end-of-frame chain (UI3DSceneManager -> RenderEndOfFrameEffects, RenderEffect), the kMAIN copy
  in DrawInterface, the frozen-background composite (AE 107130: ISCopy kMENUBG -> framebuffer, with ISCopy's +0x88 cleared around
  it), the local map's ISLocalMap. They still enter the engine's `RenderEffect`, whose steps CS draws natively from the engine's
  param rows. Natively they need the menu scene port (phase 5's menu scenes) for the scene and its HDR data; the frozen composite
  could go first (detour 107130 in native and stage the copy without RenderEffect; observe ISCopy's param rows first).
- **DrawFrame's RenderEffect x3**: #1 (a debug copy behind two flags, never seen) and #2/#3 (PrintScreen: kFRAMEBUFFER into 0x4C
  and back, then the save; S24). Decision 15 makes the image a snapshot event for CS's screenshot feature; not built.
- **Render reads above**, and `taaReset` (game-set, render-cleared; native TAA does not apply it; check whether native TAA should
  drop its history on that reset).
- **ISDownsampleCS** (image-based flares, forced off by CS: frames with IBLF keep the composites on the engine) and the no-raster
  steps (ISMap, ISNoise, taken with no pass) are unchanged.
- `sky-native` never finished `coc Riverwood` (timescale 0): the serving thread rendered the loading screen for 100 s while the
  steps ran. Not checked on the base build.
