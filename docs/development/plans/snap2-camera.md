# Snapshot roadmap phase 2: the frame camera (lead `camera`)

Branch `snap2/camera` (base `org_integration` 9a5c0ea05), stage `s2-camera`. Roadmap: [render-thread-snapshots.md](render-thread-snapshots.md),
decisions 17, 21 and 31 in [render-thread-decisions.md](render-thread-decisions.md).

## What was built

### The frame camera (`src/Snapshot/FrameCamera.{h,cpp}`)

- **Capture.** `FrameCamera::Capture` fills the main view's `CameraView` from the world root camera: placement, frustum and port
  as before, plus SetCameraData's unjittered `view`, `proj` and `viewProj` (row-major, `DCLF::ShadowCameras::CameraMatrices`, the
  existing bit-exact port). `CameraView` gained the three arrays (additive). Publication.cpp's change is only the capture call.
- **Render side.** Once per render frame, at `Main::UpdateJitter` (Upscaling's thunk), `FrameCamera::BeginRender(jitter, ratios)`
  builds two per-frame blocks (the engine's `VS_PerFrame`, b12 layout, `globals::FrameBufferCache`):
  - `Main()`: jittered, the render frame's dynamic resolution ratios. The world passes.
  - `Post()`: the same camera unjittered (the engine sets it so for the sun query and the post chain). Its ratios become 1 once
    the frame is at full resolution (`OnFullResolution`, called after the upscaler and from `Upscaling::PostDisplay`).
  - The previous camera (c16 `CameraPreviousViewProjUnjittered`, c41 `CameraPreviousPosAdjust`) is the camera this render side
    drew last, not the snapshot's previous. A repeated snapshot so renders with zero camera motion; after dropped snapshots the
    reprojection is from the camera really drawn. The first render after start uses its own camera (no motion).
  - A frame without `Main::UpdateJitter` builds lazily, unjittered, on first use.
  - `FrameParams` = (1/fGamma, 1, 0, 0) for the main view; fDRClampOffset and 1/fGamma are captured in `CoreFrame`.
- **Modes.** `Owned()` is native. Guest and plain D3D11 return `globals::game::frameBufferCached` from `Main()`/`Post()`, so guest
  reads what it read before at every converted site.
- **Shared helpers.** `WriteCamera` (c0-c41), `WriteRatios` (c43-c44), `Inverse`, `RowMajor`. DCLF's `Draws::PerFrameBlock` (first
  person, reflection faces) now calls `WriteCamera` and takes its jitter from `FrameCamera::Jitter()`; `Draws::Inverse` is gone.

### Equality proof (removed after)

A temporary compare logged, per call site, the built block against `frameBufferCached` (field by field, 1e-4 relative) for
20,000-60,000 calls in native-vulkan+quick (`s2-camera-cmp-*`, `cmp2`, `cmp3`, `cmp4`). Found and fixed:
- c20/c24 hold the *unjittered* projection's inverse and the unjittered projection (DCLF's port had them jittered); c36 is the
  jittered inverse.
- FrameParams.y is 1 for the main view.
- Where the main camera is bound, every field then matched: DCLF's main epochs (203 calls, 0 differ), LLF, fog, screen-space
  shadows, ViewAtmosphere, SharedConstants in PreMain/Deferred/Water/PostWater. The only remaining differences were the previous
  camera on the first world frame (the engine's is zero there) and, in PreMain, the engine's zero previous camera.
- The Sun and Post segments and Streamline see the main camera **unjittered** (hence `Post()`); after the upscaler and
  PostDisplay the ratios are 1. Missing that second part cropped the picture to the render-size corner (seen and fixed in
  `s2-camera-v1`, `v2`).
- Start (reflection faces), PreDepth (the last shadow view) and Ui (menu scenes) hold other cameras: they keep the engine's block.

### Converted sites

All 53 `frameBufferCached` reads but these now read `FrameCamera::Main()` or `Post()` (Effects11, ImageSpace's native steps,
SharedConstants' Sun/Post segments read `Post()`):
- left: `SharedConstants.cpp` for the Start/PreDepth/Ui segments (other views), the shadow capture's fallback
  (`ShadowCapture.cpp:211`, per shadow view, used only when the mirror is empty), `ViewEpoch.cpp` `FrameBlockNow` (the non-camera
  rows of first person and reflection faces: FrameParams, ratios), `ForwardHooks.cpp:70` (FrameParams.z after the composite, not
  camera) and the check-only log in SharedConstants.
- DCLF's main epochs: in native b12 is always the frame camera (the mirror of the engine's buffer is no longer used for b12).
- Camera reads of the engine's render state also moved in native: sky batches (`SkyNative.cpp` `Frame::Read`, main and sun views),
  water (`WaterPortCapture.cpp` `CaptureFrame`), SSGI's projection and view, SSS's FOV.
- Light Limit Fix's `FrameInputs` camera (projection inverse, view, eye) reads the frame camera (item 5).
- Near/far (decision 21): native takes them from the world camera's frustum in the capture; guest keeps the culler's copy.

### Render-owned jitter

`PostChain::UpdateJitter` returns the jitter and writes `BSGraphics::State` (+0x44/+0x48 and the table index) only outside native.
`Upscaling::ConfigureUpscaling(state, jitter&)` takes and returns it; it writes `projectionPosScale` only outside native. Native's
engine state stays unjittered (game-side picking and culling), and the jitter reaches the frame through `FrameCamera`.

### Previous transforms (item 3)

- Main camera: above.
- DCLF objects (`FrameValues.cpp` `SampleListed`): in native a row's `previousWorld` is the `world` this render side last wrote for
  the same geometry in that slot (`rowOwner` tells a continued slot from a reused one), except LOD (current transform) and first
  person. A mover is resampled while its sample changes, so a repeat writes previous = world once and then settles.
- Sky: `previousPosAdjust` from the frame camera. Water: the same (its PreviousWorld is the current world with the previous
  posAdjust).

## Runs (cs-test, stage s2-camera, Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-vulkan+quick (compare builds) | s2-camera-cmp-nvk, cmp2, cmp3, cmp4 | passed |
| native-vulkan+quick | s2-camera-v1, v1b (cropped picture), diag, diag2, diag3 | passed (bug found) |
| native-vulkan+quick | s2-camera-v2-native-vulkan-quick | passed, shot matches cmp4 but for the guard's animation |
| native-vulkan+cadence-drop | s2-camera-v3-native-vulkan-cadence-drop | passed, no smear in "start" or "turned" |
| native-vulkan+cadence-repeat | s2-camera-v3-native-vulkan-cadence-repeat | passed, 0 singular Streamline frames (64+ before) |
| native-vulkan+cadence-drop, cadence unset | s2-camera-v3-nocadence | passed (the shot baseline) |
| guest+quick | s2-camera-v3-guest | passed |
| native-d3d12-quick | s2-camera-v3-d3d12 | passed |
| guest-upscalers | s2-camera-v3-guest-upscalers | passed |
| native-upscalers | s2-camera-v3-upscalers | passed, but "taa" (a menu over the world) cropped: lazy builds kept the render ratios |
| native-upscalers | s2-camera-v4-native-upscalers | passed after the fix (lazy builds draw at ratio 1); all four shots match guest's framing (mean diff 11.9, the guard's animation) |
| native-flicker-taa | s2-camera-v3-flicker-taa | passed; mean consecutive-frame difference 2.7 over 20 pairs |
| native-vulkan+quick, CS_SNAPSHOT_DETECT=1 | s2-camera-v3-detect | passed |
| native-vulkan+quick | s2-camera-v4-native-vulkan-quick | passed (final build) |

Cadence shots against the same steps with the cadence unset (`cs_test.py compare`, mean channel difference): drop start 14.0,
turned 25.1, settled 15.2; repeat start 15.2, turned 31.2, settled 14.5. Steps count render frames, so the game state (the guard's
attack, the player's position) differs between the modes and the numbers mostly measure that; by eye neither cadence shows the
drop-mode smear seen in phase 0. A shot comparison at matching game state needs the cadence lead's game-frame steps.

### Detector (native Vulkan quick, exit table, per world frame)

`frameBufferCached` reads left in native's world frame: `SharedConstants.cpp:83` 5.53 (the Start/PreDepth/Ui segments, other
cameras), `ForwardHooks.cpp:70` 1.00 (FrameParams.z), `ViewEpoch.cpp:251` 0.01 (fallback). No read at `FrameCamera.cpp` in native.
Before, every one of the 53 sites read it as bound state at record time (the phase 1 tables list them; for example
SharedConstants once per committed step, Streamline 7 per frame).

## Decided by the agent, to confirm

- Which segments read the frame camera through SharedConstants (Main: PreMain to SunWater but Sun; Post: Sun and Post; engine:
  Start, PreDepth, Ui). From the compare: the engine's block holds another camera in those three.
- `Post()` exists because the engine itself draws the post chain and the sun query unjittered; a single jittered block would have
  changed native's post chain.
- The previous camera after a camera cut is still the last drawn one (the engine's is too); history resets are the cadence lead's
  events.
- Objects' drawn previous transforms are native only, so guest's motion vectors stay the engine's.
- First person's c20/c24 are now unjittered in both modes (the engine's layout); a sub-pixel change in guest's first person.

## Open items

- Skinned actors' previous bone palettes are still the engine's previous update (palette rows).
- First person and reflection faces still take FrameParams and ratios from the engine's mirror (`FrameBlockNow`).
- Reflection faces, shadow views and menu scenes need their own snapshot cameras (`ViewId` rows) before Start, PreDepth and Ui stop
  reading the engine's block (phase 3/5).
