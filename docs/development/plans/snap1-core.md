# Snapshot roadmap phase 1: the core group (lead `core`)

Branch `snap1/core` (base `org_integration` aba6c707d), stage `s1-core`. Roadmap: [render-thread-snapshots.md](render-thread-snapshots.md),
contract: [snap0-contract.md](snap0-contract.md).

## What was built

### The core block (`src/Snapshot/CoreFrame.{h,cpp}`)

`Snapshot::CoreFrame` (`BlockId::CoreFrame`) is the game data the shared frame state reads. `Snapshot::CaptureCore` fills it
inside `Snapshot::Capture`, after the core fields and before every feature capture, so feature captures can read it through
`a_builder.Built().Block<Snapshot::CoreFrame>()`. Record code reads `Snapshot::Core()` (a default block before the first
snapshot).

| Field | Source at capture |
|---|---|
| `cameraNear`, `cameraFar` | the high actor culler's frustum (SE 517032 / AE 403540 +0x40/+0x44), what `Util::GetCameraData` always read |
| `fovDegrees` | the world FOV global (SE 513786 / AE 388785) |
| `noSky` | `Util::IsInterior`'s rule (TES cell and worldspace flags) |
| `dirLightColor`, `dirLightFade`, `dirLightDirection` | the shadow scene node's sun light |
| `dirLightOverridden`, `sunlightScale` | `LinearLighting::IsSunlightOverridden` / `GetSunlightScale` (Sky Sync's override, the ImageSpace HDR scale) |
| `dalc` | `BSShaderManager::State::directionalAmbientTransform` |
| `hasSky`, `hideSky`, `sunDirection`, `masserDirection`, `secundaDirection`, `sun`, `masser`, `secunda` | the sky, `SkySync::GetCelestialDirection`, the moon phase colours with Sky Sync's settings |
| `noScatteringCloud`, `secundaMoonMesh`, `secundaShadowMesh` | geometry keys (never dereferenced) for `State::UpdateSkyShaderPermutation` |
| `water[25]` | water colour and relative height in the 5x5 cells around the main camera (`Util::TryGetWaterData(eye, x, y)`) |

The sun light, DALC, sky and Sky Sync's directions are written by `Sky::Update` in the game update (Sky Sync detours it), so
they are current at the capture point (Main::Update's call of Main::DrawFrame).

### State::UpdateSharedData and ModifySharedLighting

`State::UpdateSharedData(bool a_inWorld)` is now a function of `Snapshot::Core()`, CS settings and render state (pre-exposure,
upscaler, frame counter): it reads no engine object. The unused `a_prepass` flag is gone (call sites in `Deferred.cpp` and
the one in `Upscaling.cpp`). The display size (`BufferDim`, `MipBias`) comes from the adopted `DisplaySize` frame variable
(`Util::DisplaySize()`), the size graph textures are made for.

`SharedLighting` carries `directionalOverridden` instead of the light pointer. `LinearLighting::ModifySharedLighting` applies
`LightColorToWorking`'s rule from it: an overridden colour stays, otherwise `SRGBToWorking`. Same result as before for the
directional light (it is never a point light, so the "linear" light flag never applied).

### Util helpers (`src/Utils/Game.{h,cpp}`)

One function, two producers, told apart by the detector's capture scope: inside a capture (`EngineReads::InCapture()`) the
helper reads the game; anywhere else it reads the adopted core block. Callers keep their calls; the other leads' features
need no edit.

- `IsInterior`, `GetCameraData`, `GetWorldFOVDegrees` (new), `GetVerticalFOVRad` (FOV from the block, aspect from
  `DisplaySize()`).
- `TryGetWaterData(eye, x, y)`: capture only (its one caller is the core capture).
- `GetScreenDispatchCount`: the display from `DisplaySize()`.
- `GetTemporal`: CS's own copy of what `SetTemporal` last set (the engine's flag before the first set). Only CS writes the
  flag, so the frame no longer reads ImageSpaceManager for it.
- `Util::ColorSpace::LightColor` got a defaulted `operator==` for the block.

### Settings revision (decision 17's follow-up)

`FrameSnapshot::settingsRevision` is now `CS::SettingRegistry::Serial()`, a counter every setting raise moves (`Raise`,
`RaiseAt`), read when the snapshot is built. It is game-side and works without the render graph (the old value, the
revision's taken-variables serial, was 0 in plain D3D11 and moved only at a render Begin). The INI values the frame reads in
my files: none at record time. `State::Reset`'s `bEnableImprovedSnow` and `Util::DisableVanillaTAA` are writes, left.

## Detector (native Vulkan quick, autoloaded save, Dev-Fast, CS_SNAPSHOT_DETECT=1, exit table)

Live reads per world frame at sites in my files (State, Deferred, Utils, Snapshot, Frame/{Variables,PostChain,FrameDescription},
LinearLighting):

| Run | Mine | Whole frame |
|---|---|---|
| before (`s1-core-nvk-before2`) | 308.1 | 1723.7 (record-time 839.7) |
| after (`s1-core-nvk-after`) | 80.8 | 1387.2 (record-time 633.6) |

Before, the largest: `State.cpp:137` sky 51.3, `Game.cpp:187` eye 33.7, `Game.cpp:122/123/169` water 25+25+20,
`PostChain.cpp:372` 22.5, `Game.cpp:423` IsInterior 19.4, `Game.cpp:241` ConvertToDynamic 12.1, UpdateSharedData's own
capture-point sites about 45, `Game.cpp:207/216` TAA flag 11.2.

After, what is left and why:

| Site | Per frame | Why left |
|---|---|---|
| `PostChain.cpp:372, 120, 254, 284` ImageSpace | 32.4 | ImageSpace effect selection and flags, set by the engine's ImageSpaceManager in the render half. Phase 5 computes them from game data |
| `Game.cpp:282` ConvertToDynamic | 12.1 | dynamic-resolution ratios and lock: CS's own render state kept in `BSGraphics::State`, written mid-frame by Upscaling (post lead), Skylighting, DCLF and the precipitation/facegen thunks. A snapshot value would be wrong; it moves with Upscaling's port to the `DynamicRatios` frame variable |
| `D3D.cpp:58` GetCurrentSceneDepthSRV | 6.1 | the engine's depth targets: engine-port glue (EngineTargetSet, Skeleton) |
| `Game.cpp:189` GetEyePosition | 5.5 | `posAdjust` is per view (cube faces, shadows) and written by the engine's render half; callers are other leads'. Needs the frame-camera port |
| `Game.cpp:233` TAA shader | 5.0 (other) | `SetTemporal`'s write of the engine flag (Upscaling, Hooks) |
| `Deferred.cpp:702` sun shadow cascades | 3.8 | `sunShadowDirLight` split distances and cascade matrices from `CalculateAndDrawShadowCasterLights`: shadow bookkeeping, phase 3 |
| `LinearLighting.cpp:424, 340` | 4.0 | the per-draw SetupGeometry hooks (engine draws), not ModifySharedLighting |
| `Deferred.cpp` 258, 293, 336, 372, 386, 390, 575, 665, 683, 944 | 10 | Deferred driving the engine renderer: render targets and modes in the shadow state, dirty flags, blend states, the engine's perFrame buffer, depth copies. Engine-port glue that phases 3-5 replace |
| `State.cpp:425, 427, 451` | 2.6 (other) | `State::Reset` at Present: menu flags, the timer, ApplyReflections off. Phase 2 splits Reset |

Not done in phase 1 (asked by the sky and post leads): the frame-buffer camera (`frameBufferCached` matrices, posAdjust,
`DynamicResolutionParams1`). It is the engine render half's per-view output plus CS's jitter; it needs its own port. Both
leads leave those sites marked "waiting on the frame-camera port".

## frameCount uses I touched (phase 2 input)

| Use | Means |
|---|---|
| `State::UpdateSharedData`: `preExposureFrame != frameCount` (rotate previous pre-exposure once) | render frame |
| `SharedDataCB::FrameCount`, `FrameCountAlwaysActive` (temporal noise, TAA phase) | render frame |
| `Deferred::CopyShadowLightData` / `CaptureDirectionalShadowMap` / `GetDirectionalShadowMap` (data and map from the same frame) | render frame |
| `Util::FrameChecker` (`Game.cpp`, IsNewFrame / ShouldSample) | render frame |
| `LinearLighting::LightColorToWorking` memo key | render frame (a cache key) |

None means "new game frame". The block itself is game state: a repeated snapshot renders it unchanged, a dropped one
loses nothing (no integrator in it).

## Runs (cs-test, stage s1-core, Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-vulkan+quick, detector | s1-core-nvk-before | failed: screenshot readback lost (baseline build, lab under load; rerun passed) |
| native-vulkan+quick, detector | s1-core-nvk-before2 | passed (baseline) |
| guest+quick, detector | s1-core-guest-before | passed (baseline) |
| guest+quick, detector | s1-core-guest-after | passed (1337 live reads/world frame vs 1385) |
| native-vulkan+quick, detector | s1-core-nvk-after | passed |
| native-d3d12-quick, detector | s1-core-d3d12 | passed |
| native-vulkan+cadence-repeat | s1-core-repeat | passed (3 shots) |
| guest, custom steps (tgm, timescale 0, Dragonsreach shot, Whiterun shot) | s1-core-guestcmp-after, s1-core-guestcmp-before | passed both; same picture: Whiterun exterior differs only in the player's idle pose (6% of pixels over 8), Dragonsreach in NPC and player placement, with the same lighting, ambient, shafts and exposure |

The quick save is a fight at night (the "start" shot differs between any two runs), so the guest comparison uses the custom
steps above with time stopped.

## Decisions by the agent, to confirm

- **One helper, two producers by capture scope** (`InCapture()`), not separate capture and record functions: callers in other
  leads' features need no edit, and a capture that calls the helper reads the game. Game-thread callers outside any capture
  (none found among the changed helpers' callers that run before the capture) would read the previous adopted snapshot.
- **Near/far from the high actor culler's frustum**, as before: the culler copies the world camera's frustum in
  `Main::SetupSunAndSky` (render half). Captured before DrawFrame it is the previous render's copy, which equals the
  camera's unless near/far changed this update (one frame late then). When native stops running the render half, take the
  world camera's frustum instead (`cameras.Current(Main).frustum`).
- **Water tiles from the main camera's position** (the snapshot's `Main` camera), not `posAdjust`: the main view's posAdjust
  is that camera's world translate.
- **Display size from the `DisplaySize` frame variable** instead of `BSGraphics::State`: the same value outside a resize,
  and during one it matches the graph textures.
- **The settings revision counts setting raises**: settings that are plain struct fields (not `CS::Setting`) do not move it;
  they reach it as features move to bound settings.
- **VR**: `Snapshot::InstallHooks` does nothing on VR, so the block stays default there. The SE/AE build targets do not
  include VR.

## Open items

- Frame-camera port (frame-buffer matrices, posAdjust per view, jitter applied to the snapshot's projection).
- Dynamic-resolution state as a frame variable owned by Upscaling (then `ConvertToDynamic` reads it).
- PostChain's ImageSpace reads (phase 5), Deferred's engine glue (phases 3-5), sun shadow cascades (phase 3).
