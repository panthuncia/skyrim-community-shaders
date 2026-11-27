# snap7-precip: precipitation occlusion without render-time engine reads

Lead `precip`, round `snap7`, branch `snap7/precip` (base `org_integration` ca9035fe0), stage `s7-precip`. The precipitation item of
[snap7-reads.md](snap7-reads.md) section 5 (decision 212). Still today's single-threaded frame.

Status: **done**. In native's render half the precipitation occlusion sites read no game-mutated engine state and make no engine call.

## 1. The engine function

`Precipitation::ComputeProjection` (SE 25643 at 0x1403af530, AE 26185 at 0x140408520; Ghidra names it `UpdateProjection`). Its only
callers are render side: Main::Precipitation::RenderOcclusion (AE FUN_1404076c0) and SetupMask. It does not read `this`. Inputs:

- the precipitation shader's direction (SE 515509, AE 401648) and cube size (SE 515451, AE 401590), both written by
  `Precipitation::Update` (AE FUN_140407b60) inside `Sky::Update`, on the game thread;
- the world camera (`SceneGraph::camera` of the world root): world translate and forward (rotation column 0);
- the occlusion camera's own world forward before the call (the engine places the camera from its previous forward);
- the occlusion camera's fixed fields: port, scale, and `minNearPlaneDist` / `maxFarNearRatio`, which `NiCamera::SetViewFrustum`
  (AE 0x140d2b0e0) clamps the near plane with.

It builds the basis d (direction, or (0,0,-1) when |d|1 < 0.001), a = unit(d x UNIT_Y), b = unit(a x d), snaps the point half a cube
ahead of the world camera, less two cubes along the previous forward, to a grid of cube/128 along each basis vector (truncate, step back
when negative), sets local rotate (columns d, b, a) and translate, an orthographic frustum (+-cube/2, near 1, far 4 cube) through
SetViewFrustum (Skylighting's quarter hook sits on that call), and updates the camera (no parent: world = local). Both runtimes group
every sum the same way (checked from both disassemblies). `Precipitation::RenderMask` (AE 26184) then writes the occlusion projection:
identity rows (AE 196452-196454, constant data) with the eye's offset from the camera, times the camera's view-projection.

## 2. What changed

- **`Snapshot::PrecipitationOcclusion`** (`src/Snapshot/PrecipitationOcclusion.{h,cpp}`, new block `BlockId::PrecipitationOcclusion`,
  registered in `Snapshot::InstallHooks`, both modes fill it): `Compute` (the port, a function of `ProjectionInputs`) and `Projection`
  (RenderMask's projection). The capture takes the sky's precipitation, its geometry, the engine mask test (occlusion map on, rain or
  snow: `engineMask`), whether the geometry has a rain emitter, the camera's fixed fields, and computes the **mask's camera and
  projection once per update** from the update's direction, cube size and the snapshot's main camera. The previous forward is CS's own
  game-side state (the last update's mask forward, seeded once from the engine camera). Native also writes the projection into the rain
  emitter there (the engine's particle setup reads it), instead of in the render half.
- **Skylighting's sky map** (native): `RenderSkyOcclusionMap` computes the map's camera (`occlusionCamera`) and projection with
  `Compute(direction, occlusionDistance or the zenith extent, previous forward = direction, the snapshot main camera's eye, the block's
  fixed fields, QuarterFrustum)`. The direction stays the render frame's sample (BeginOcclusionFrame, as before). No shader-global,
  `lastCubeSize`, accumulator or engine camera writes and no ComputeProjection/RenderMask calls in native. `QuarterFrustum` is shared by
  the SetViewFrustum hook (guest) and the native path.
- **Callers read the block in native**: `Skylighting::RenderOcclusionAtShadowMaps` (was Skylighting.cpp:819) and `RenderOcclusion`
  (:873), DCLF's `RenderPrecipitationAtShadowMaps` and its RenderOcclusion hook (`EngineMaskObject` is guest-only now). Native's mask
  work is DCLF's view only (`OcclusionView`).
- **DCLF's occlusion camera is values**: `Api::OcclusionCamera` holds world, frustum and port; `IndirectDraws::OcclusionView` takes
  them (ShadowCameras' value overload). `OccluderRules.cpp` (was :82): `PrecipitationCamera` reads the block's mask camera in native,
  the new `SkyCamera` reads Skylighting's `occlusionCamera`; guest converts the engine camera as before.
- **Wetness** (was WetnessEffects.cpp:984): native reads `maskProjection` (when `rainEmitter`).
- `DrawcallLimitFix::RenderMask` is gone (its native branch was the projection now in `Projection`; guest calls the engine's).

Guest takes every engine path it took before; the block is filled but not read there.

## 3. Observe, then switch

The observe build (s7-precip-obs-nvk-rain, native rain with Skylighting and Wetness on) ran the old engine sequence beside the new values
at each map and compared bitwise (camera rotate, translate, scale, frustum, port, projection; and the world root camera against the
snapshot main camera):

- sky map: 900+ checks, 0 mismatches (bit-exact);
- mask: 600+ checks, 1 mismatch: the first frame the rain object existed after `fw`. The engine's previous forward came from the sky
  map's restore call of the previous render frame, CS's from the previous update's mask; they differ only on that first frame
  (translation off by one grid snap, 585 units in x). Every later frame matched bitwise;
- world camera vs snapshot main camera: 0 mismatches. The occlusion camera has no parent.

The scaffolding is deleted.

## 4. Detector numbers (native, Dev-Fast, render half record, per world frame)

| Site | before (snap7-reads c, quick) | after (quick and rain) |
|---|---|---|
| WetnessEffects.cpp:984 (Sky) | 2.00 | 0 |
| Skylighting.cpp:819, :873 (Sky) | 2.00 | 0 |
| OccluderRules.cpp:82 (Sky) | 1.00 | 0 |
| record, game-mutated, quick | 10.9 | 5.79 |

Left in the record half (quick): WaterNative.cpp:675 3.00, FrameView.cpp:291 1.00, SkyNative.cpp:1035 1.00, Surfaces.cpp:122 0.75 (all
DCLF scene work, snap7-reads section 5). Not counted by the detector but also gone from native's render half: the raw precipitation
globals and the walks through `sky->precip` (camera, accumulator, geometry, emitter).

## 5. Cadence

The mask camera, its projection and the rain emitter's value are the update's: a repeated snapshot renders the same values; the previous
forward steps once per update. The sky map's direction and quarter stay per render frame (Skylighting's own frame state, unchanged).

## 6. Runs

Stage `s7-precip`, Dev-Fast. Native runs with `CS_SNAPSHOT_DETECT=1`; the rain runs with Skylighting and Wetness Effects forced on
(`--set features.Skylighting=true --set features.WetnessEffects=true`), which makes native-vulkan+rain the Skylighting and wetness
config too (no separate wetness config was added).

| Config | Label | Build | Result |
|---|---|---|---|
| native-vulkan+rain | s7-precip-obs-nvk-rain | observe | passed; sky map bit-exact, mask 1 first-frame mismatch (section 3) |
| native-vulkan+quick | s7-precip-native-vulkan-quick | final | passed with the shot reference; record game-mutated 5.79 |
| native-vulkan+rain | s7-precip-native-vulkan-rain | final | passed (2 shots); record game-mutated 5.98, no precipitation site |
| native-vulkan+skylighting-reset | s7-precip-native-vulkan-skylighting-reset | final | passed (3 shots, interior and back) |
| native-vulkan+update-cap30 | s7-precip-native-vulkan-update-cap30 | final | passed (3 shots) |
| native-vulkan+update-cap240 | s7-precip-native-vulkan-update-cap240 | final | passed (3 shots) |
| native-vulkan+cadence-drop-cells | s7-precip-native-vulkan-cadence-drop-cells | final | passed (3 shots) |
| guest+quick | s7-precip-guest-quick | final | passed with the shot reference |
| guest+rain | s7-precip-guest-rain | final | passed (2 shots) |

Skipped: native-d3d12-quick (no ORG, BasicRHI or native device code changed).

## 7. Decided by the agent, to confirm

- PR-a. **The sky map is computed on the render side from the block**, not in the capture: its direction and quarter are Skylighting's
  per-render-frame sample (CS state, no engine read), and moving them to the update would halve the probe samples at a 30 Hz cap. Only the
  game inputs (world camera, the occlusion camera's fixed fields) come from the snapshot.
- PR-b. **The mask's previous forward is CS's game-side state** (the previous update's mask forward). The engine's came from whichever
  render call ran last (Skylighting's restore call). Equal on every frame but the first after the direction jumps (section 3).
- PR-c. **Native writes the rain emitter's projection in the capture** (game thread) for the engine's particle setup, every update with
  a precipitation geometry, instead of only when the mask rendered.
- PR-d. **The accumulator's camera is no longer set in native** (what SetupMask sets): native makes no engine draw into it.

## 8. Proposed decision-log entry

- **Precipitation occlusion reads captured values** (`Snapshot::PrecipitationOcclusion`): ComputeProjection (SE 25643, AE 26185) and
  RenderMask's projection are ported as functions of values (bit-exact against the engine in the observe run). The capture computes the
  mask's camera and projection once per update and native sets the rain emitter there; Skylighting computes the sky map's camera from the
  block with its frame direction; DCLF's occlusion views take camera values; Wetness reads the block. Native's render half makes no
  ComputeProjection or RenderMask call and writes no precipitation globals. Quick record-half game-mutated reads 10.9 -> 5.8.
