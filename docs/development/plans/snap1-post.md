# Snapshot roadmap phase 1: the post group (lead `post`)

Branch `snap1/post`: one commit on top of the `core` lead's b3cc9874b (`Snapshot::Core()`, `Util::DisplaySize()`), which is on
`org_integration` aba6c707d. Merge `snap1/core` first. Roadmap: [render-thread-snapshots.md](render-thread-snapshots.md). Contract: [snap0-contract.md](snap0-contract.md).

## What changed, by feature

Blocks added to `Snapshot::BlockId` (after `CoreFrame`, under "the post group"): `GrassCollision`, `UnifiedWater`,
`DynamicCubemaps`, `ScreenSpaceShadows`, `PostProcessing`. Each has a `Capture(Snapshot::Builder&)` registered in the feature's
`PostPostLoad`.

- **Grass Collision.** The collider gathering (process lists, the player, each actor's collision shapes) was a Main::Update
  hook after DrawFrame (`MainUpdate_QueueCollisions`, removed). It is now the capture: `GrassCollision::Block` holds the boxes
  and shapes, relative to the snapshot's main camera position, which is also the eye the frame's update uses now (it was the
  world root camera's translate read at EarlyPrepass, and the shapes were relative to the previous update's posAdjust).
  The collision decay (`TimeDelta`) steps once per new snapshot by `events.gameDelta`. `GrassShared::Collision::frame` was
  unread and is gone.
- **Unified Water.** `Block::exteriorActive`. The water LOD root's cull flag is game state and is written in the capture
  (it was written from the render half's `TESWaterSystem::UpdateDisplacementMeshPosition` hook). The render-half hooks
  (displacement offsets, `BSWaterShader::SetupGeometry`) read the block. The terrain LOD visibility hook
  (`BGSTerrainManager::Update` -> `UpdateWaterMeshSubVisibility`, checked in Ghidra) and the block attach are game-side logic
  (decision 8): their sites are now category Other.
- **Dynamic Cubemaps.** `Block::skyHidden`; the game-time jump test reads `gameDaysPassed` from the snapshot.
- **Screen Space Shadows.** `Block::skyFull`; the sun direction is the core block's `dirLightDirection`; the dynamic
  resolution ratio is CS's own copy (`Upscaling::FrameResolutionRatio`, new); the D3D11 path's size is `Util::DisplaySize()`.
- **Post Processing.** `PostProcessing::Block`: the ImageSpace data (with the override/current base data as its base), the
  Cinematic Camera's published state, and the focus targets (the Cinematic Camera's Target mode with its held history,
  DoF's own target focus, the console pick, RaceMenu's player). The Cinematic Camera's update moved into the capture: it
  writes the game's world FOV, so it is game-side logic. DoF and Physical Lens read the block instead of calling the focus
  resolver at record time. `GetActivePhysicalCameraState` reads the block.
  Visual-only clocks (Physical Glare's tear film, Physical Lens's cutoff release) step by `Snapshot::VisualDelta()` (new in
  `Snapshot/Publication.h`: the render delta, 0 while paused). Histogram auto exposure takes the snapshot's `realDelta` (the
  same value BSTimer gave); phase 2 moves it to the render delta.
- **Upscaling, Frame Generation, Streamline.** Display size from `Util::DisplaySize()`; pause and menu tests from the
  snapshot's `menus`; Streamline's near/far from the core block. The jitter write (`ConfigureUpscaling`), its undo
  (`PostDisplay`), `PerformUpscaling`'s lock and the lock writes around the precipitation and FaceGen engine calls stay
  (phase 2's render-owned jitter, phase 3's ports): they are writes, now category Other.
- **Effects 11.** `Effects11::Block`: the sky (mode, kHideSky, weather pct, game hour, horizon colour, the effective weather
  ids after Location Weather, with the outgoing weather held past its transition), the sun (visibility, disc blend, Sky
  Sync's celestial direction, the root offset), Sky Sync's moon light (caster, colour, direction), the sky bodies'
  billboard tangents, the world FOV, the ImageSpace data, and the two INI values (`bUseMultipleLuminanceReferences`,
  `fReinhardWhiteScale`, no longer function-local statics at record time). The common data, sky scattering, volumetric and
  sun rays, ENB adaptation's sun mask and the ENB effect variables read it. Visual-only clocks (the effects' Timer, ENB
  adaptation, ENB depth of field) step by `Snapshot::VisualDelta()`. TextureManager's display size from
  `Util::DisplaySize()`. `ENBHelper` was dead (it filled caches nobody read) and is deleted.
- **Light Limit Fix.** Near/far from the core block; the D3D11 grid's display size from `Util::DisplaySize()`.
- **Terrain Blending, SSGI.** Their engine-state writes (the depth pass's `SetCameraData`, dirty flags, the terrain draws'
  blend/depth modes, the vanilla SSAO toggle) are category Other now.
- **HDR Display.** The menu/pause test reads the snapshot's `menus`.
- Engine state writes that were tagged RecordTime are now Other: the post chain's `DIRTY_RENDERTARGET` sets, the vanilla
  lens flare INI write on a settings change.

## Measurements (live-read detector, native Vulkan, autoloaded save, Dev-Fast)

Per-site reads per world frame for the group's files (non-"other" categories), from the exit table.

Equality was proven before switching (temporary compares, one guest and one native Vulkan run, then removed): the
ImageSpace block against the live manager, the core block's sun direction against the accumulator's sun, and
`Upscaling::FrameResolutionRatio` against the engine's ratio: 0 differences in 100 frames each.

Native Vulkan quick, group files, per world frame (`s1-post-nvk-before` -> `s1-post-native-vulkan-quick-b`):

| Feature | record-time | capture-point | bound-state |
|---|---|---|---|
| Upscaling, Streamline, FG | 12.0 -> 2.0 (target fields) | 2.0 -> 0 | 7.0 -> 7.0 (frame camera) |
| Grass Collision | 7.0 -> 0 | | |
| Unified Water | 6.8 -> 0 | 5.4 -> 0 | |
| Terrain Blending | 7.0 -> 4.0 (target fields) | 3.0 -> 1.0 (target fields) | |
| Post Processing | 3.6 -> 0.6 (target field) | | |
| HDR Display | | 4.2 -> 0 | |
| Screen Space Shadows | 3.0 -> 0 | | 1.0 -> 1.0 (frame camera) |
| Light Limit Fix | 3.0 -> 1.0 (light walk) | 3.8 -> 8.6 (engine Lighting hooks, varies with the scene) | 3.0 -> 3.0 (frame camera) |
| SSGI, SSS | 2.9 -> 1.9 (shadow-state camera) | | |
| Dynamic Cubemaps | 1.0 -> 0 | | |
| Volumetric Shadows | 1.0 -> 1.0 (target field) | | |
| Total | 50.3 -> 10.5 | 18.3 -> 9.6 | 11.0 -> 11.0 |

Whole frame (all features, same save): 1625 -> 1349 live reads per world frame (record-time 816 -> 600). Native D3D12
quick shows the same remaining sites. Post-effects: every PostProcessing record-time read is gone but the kMAIN target field
(PostProcessing.cpp:834). Effects 11 could not be measured: its config ends in a device loss about 860 frames after
world-ready on the base build too (`s1-post-fx-before`), before the exit table.

Every remaining record-time or bound-state read in the group is one of: an engine target field (CS's own handle), the
frame-buffer or shadow-state camera matrices, LLF's light walk and local-shadow code, or LLF's per-pass engine hooks.

## Decisions by the agent, to confirm

- **Frame-buffer camera matrices stay bound-state reads** (`frameBufferCached`: Streamline's reprojection constants,
  LLF's cluster matrices and eye, Screen Space Shadows' light projection, Effects 11's sun rays/mask/extern bindings), and so
  do the shadow-state camera matrices (SSGI's projection and previous inverse view, SSS's FOV). They are the engine render
  half's per-view output plus jitter. The `core` lead does not provide them in phase 1; they need a frame-camera port
  (view/projection from the snapshot camera and render-owned jitter) shared by every feature. Proposed as its own item.
- **Engine target fields** (`renderer->renderTargets[...]`, `depthStencils[...]`) hold CS's own tripwire handles in native;
  reading them is not reading game state. Left as RecordTime so they stay visible; they go when CS exposes the handles
  from its target set (`EngineTargetSet`) or the engine-driven call sites go (phases 3-5).
- **Light Limit Fix's light gathering stays at record time.** It walks the shadow scene's lights, but each light's shadow
  data (mask channel, `IsShadowMapRendered`, the local shadow cache's slices) is written by the engine's render half and
  DCLF's shadow pass after the capture point. Splitting it needs native shadow caster selection (phase 3). Its near/far
  now come from the core block. `ORGLightCulling::FrameInputs` is not folded yet
  for the same reason: half of it (matrices) is frame-camera output.
- **Terrain Blending's sites are engine-state manipulation during the engine's terrain draws** (depth SRV swaps, blend and
  depth modes, dirty flags, `SetCameraData`). They exist because the engine drives the depth pass; they go with phase 3.
- **Grass collisions are gathered one update earlier than before** (at the capture, before DrawFrame, not after it), and
  are relative to the snapshot camera. On a repeated snapshot the same colliders are applied again with zero decay.
- **Visual-only animation by render delta, held while paused**: Physical Glare's tear film, Physical Lens's cutoff
  release, Effects 11's Timer, ENB adaptation and ENB depth of field. Proposed decision-log entry: "Visual-only post
  animation (glare tear film, lens cutoff release, ENB adaptation, ENB DOF focus, the FX Timer) advances by
  `Snapshot::VisualDelta()`: the render delta, 0 while the snapshot is paused."
- **World state once per snapshot**: grass collision decay. Proposed entry: "Grass collision springs back by
  `events.gameDelta`, once per new snapshot."
- **Auto exposure** keeps the wall-clock delta for now (`snapshot->realDelta`, the value it read from BSTimer); phase 2
  moves it to the render delta (roadmap watch list).
- **The Cinematic Camera's FOV write runs in the capture** (decision 8: game-side logic stays on the game thread).
- `Snapshot::VisualDelta()` lives in `Snapshot/Publication.h` (the contract) so every group can share it.
- Small blocks hold values the core block also has in another form (`DynamicCubemaps::Block::skyHidden` vs
  `CoreFrame::hideSky`): fold at merge if the director prefers one source.

## Left (not done in this pass)

- **Frame-camera port** (above): Streamline's reprojection, LLF's matrices and eye, SSShadows' light projection, SSGI's and
  SSS's projection, Effects 11's sun rays, sun mask and extern bindings. Then fold `ORGLightCulling::FrameInputs` into a
  block.
- **Light Limit Fix**'s light walk and local shadow cache code (`UpdateLights`, LightLimitFix.cpp:1486-1527, 1991, 2145):
  needs phase 3's native shadow casters. Its per-pass `BSLightingShader` hooks go with the engine draws.
- **Engine target fields** in Upscaling (Upscale, UpscaleDepth, FG), Post Processing, HDR, Terrain Blending, Volumetric
  Shadows, DoF/Physical Lens/Glare/Motion Blur/Border's D3D11 paths, Effects 11 (ENB effects' TextureOriginal, the luminance
  RTV, ORGSunRays' depth): a CS accessor for the handles, or the end of the engine-driven sites.
- **Effects 11**: `GradeSkyColors`/sky colour hooks still take a live `RE::Sky*` from the engine's sky update; the Ghidra
  check of whether that hook is game-side was not done. ORGFxRuntime's display-size fallback reads graphicsState when the
  frame variable is unset (startup only).
- **Grass Optimizations / GrassBucketCulling, IBL, Extended Materials, Terrain Variation, Water Cache**: no record-time
  reads in native's world frame (their sites are D3D11-path, load-time or Other). DCLF's grass files are DCLF's.
- **UnifiedWater and DCLF's `WaterPort::CaptureFrame`**: left to DCLF. It would fold in as a `WaterPort` block filled by
  the same capture loop (its inputs are TESWaterSystem's water objects and the cell water heights, game data at the
  capture point); `UnifiedWater::Block::exteriorActive` would join it.
- **ColorGrading's VS b1**: only the D3D11 path saves and restores the bound buffer; native's ORG path never reads it.

## Runs

Stage `s1-post`, Dev-Fast, all with `CS_SNAPSHOT_DETECT=1`.

| Config | Label | Result |
|---|---|---|
| native-vulkan+quick (base) | s1-post-nvk-before | passed |
| guest+quick (base) | s1-post-guest-before | passed |
| native-vulkan+effects11 (base) | s1-post-fx-before | device-lost at exit (pre-existing) |
| native-vulkan+post-effects (base) | s1-post-pp-before | passed |
| guest+quick, native-vulkan+quick (with the temporary compares) | s1-post-guest-a, s1-post-nvk-a | passed |
| guest+quick | s1-post-guest-quick-b | passed |
| native-vulkan+quick | s1-post-native-vulkan-quick-b | passed |
| native-d3d12-quick | s1-post-native-d3d12-quick-b | passed |
| native-vulkan+post-effects | s1-post-native-vulkan-post-effects-b | passed |
| native-vulkan+effects11 | s1-post-native-vulkan-effects11-b | device-lost about 857 frames after world-ready, as on the base |
| native-vulkan+upscalers | s1-post-native-vulkan-upscalers-b | passed |
| native-vulkan+hdr | s1-post-native-vulkan-hdr-b | passed |
| native-vulkan+llf-local-shadows | s1-post-native-vulkan-llf-local-shadows-b | passed |
| native-vulkan+cadence-repeat | s1-post-native-vulkan-cadence-repeat-b | passed |

Guest shots: before vs after differ by a mean of 0.94 per channel; two runs of this branch (`-a`, `-b`) differ by 0.92, so
the difference is run-to-run noise (eye adaptation, NPCs). The post-effects and effects11 shots that differ more (dof-on,
rays-on, no-bloom, mean 3-3.6) show a different game state: a hostile guard hit the player and the "Health Low" tutorial
box is up in the after run.
