# snap3-depth: native RenderDepth and RenderShadowMasks without engine calls (phase 3)

Lead: `depth`, branch `snap3/depth` (base `org_integration` 175c008da), stage `s7-depth`. Phase 3 of
[render-thread-snapshots.md](render-thread-snapshots.md); decisions 8, 89, 95, 97, 117, 122-127 of
[render-thread-decisions.md](render-thread-decisions.md). Same method and driver shape as `snap3-accum.md`.

| Item | State |
|---|---|
| Main::RenderDepth: no engine function called in native | done (`RenderGraph/Engine/Native/RenderDepth.cpp`) |
| Main::RenderShadowMasks: a CS-driven pass, no engine call | done (same file; DCLF `ShadowMask::DrawWorldMasks`) |
| First-person view inside RenderDepth: what native needs | decided (below) |

## 1. What the engine functions do (AE 1.6.1170 decompiles; SE 1.5.97 has the same structure)

**Main::RenderDepth** (AE 107139 0x1414ccb30, SE 100421):
1. `SetCurrentDepthStencilTarget(kMAIN, SRTM_CLEAR)`, RT0-2 and the cube target unbound, stencil off, flag 411398 = 1.
2. `StartAccumulating(world camera, depth accumulator 415008, 0x21)`: SetCameraData, the current camera (414580), then the
   accumulator's FinishAccumulating (vfunc 0x2A), which for the depth accumulator's render mode jumps to AE 106592: the depth
   batches and geometry groups inside a depth shift window.
3. `ClearItemsList`, `RenderFirstPersonView_End`: DCLF's hook here runs the Z-prepass (`DepthCommitted`).
4. With focus shadows on (390932) and focus targets listed (414625): per focus target, AE 106569 registers the target's geometry
   into the accumulator, then `StartAccumulating(..., 1)` with stencil reference index + 1 (the focus shadows' stencil marks),
   inside a narrowed near plane and depth range.
5. In first person: the depth range set to f1stPersonFarDepthRange, the first-person root unhidden, a culling process over it
   (BSCullingProcess::Process), the first-person camera and every node under the root shifted by the root's translation
   (AE 107150, twice: undo then redo), `SubmitAccumulator(first-person camera, 3)` (DCLF's first-person draw hook), the root
   hidden again, the depth range restored.
6. The copy of the main depth into kPOST_ZPREPASS_COPY (the skeleton's "RenderDepth copy" site), and the end state.

**Main::RenderShadowMasks** (AE 107140 0x1414cd600, SE 100422): with the scene's shadows on (390933) or a caster kept last frame
(415035): `SetClearColor(sunOff ? 0 : 1, 0, 0, 0)`, `SetCameraData(world camera, 1)`, the current camera; with the scene's shadows
on, the world masks (AE 107141, DCLF's MaskBatch hook: EndTechnique, the targets, the clear by flush, the draws, ReturnShadowmaps
per light, the write mode); then the clear colours restored.

## 2. Observe (`s7-depth-b-observe-pov`, third and first person, scaffolding removed)

Shadow state and BSGraphics::State were diffed across both functions in every native world frame, with the passes the engine
offered, the skeleton calls inside, the mask build's inputs and the first-person root's transforms.

- Passes offered inside RenderDepth: 0 before and 0 after the Z-prepass point, every frame (the depth accumulator is empty in
  native; decision 89). No CS point fires inside StartAccumulating.
- BSGraphics::State: no difference across either function.
- Skeleton calls: per frame exactly one main depth clear (depth | stencil), one depth copy (RenderDepth), one kSHADOW_MASK clear
  (masks). Nothing else.
- Mask build: the starting modes were constant (fill 0, cull 1, depth bias 0, scissor 0, blend 0, alpha to coverage 0, write 1,
  extra 0); the camera RenderShadowMasks set was the world camera, its near and far equal to the snapshot's main frustum, and its
  posAdjust equal to the frame camera's, in every frame. No technique was open at the masks' call.
- First person: the first-person root sat at the origin at RenderDepth's entry and exit in every frame. Main::RenderPlayerView's
  AE 107123 (FUN_1414cb020) already does the same camera setup, node shift and shader state writes before RenderDepth, and
  RenderDepth's block undoes and redoes them with the same values: its net effect is nil.
- Net shadow state left (non-first-person frames; in first person RenderDepth also leaves the first-person camera, which
  RenderShadowMasks sets back): after RenderDepth the main depth bound (no-clear mode), no colour target, the viewport of the main
  depth, no stencil, the write mode 1; after the masks kSHADOW_MASK as target 0 (no-clear), no depth target, the write mode 1.

## 3. The drivers

`Native::RenderDepth(firstPerson, controlDriven)`, called from DCLF's Main::RenderDepth thunk in native (the innermost call-site
thunk: DCLF installs before Terrain Blending, whose thunk wraps it and keeps running):
- the main depth clear through `Skeleton::ClearDepth` (hosted by the Z-prepass epoch, as before);
- `DrawcallLimitFix::DepthPassDrawn()` (the Z-prepass with the views' depth work; the hook calls the same function in guest);
- in first person with a first-person node, `FirstPerson::DepthDrawPoint` (hidden bits and depth range, as the hook did);
- the depth copy through the new `Skeleton::CopyDepthPassDepth`;
- `LeaveDepthState` (the shadow state above, dirty-marked, viewport from `Family::FrameViewport` of the main depth).
Inside it (`WorldPass::RenderDepthScope`) DCLF's depth capture applies no engine state and `CaptureBindings` takes the main depth
from CS's frame (no colour targets).

`Native::RenderShadowMasks()`, called from a new call-site thunk on Main::RenderPlayerView's call (SE/AE +0x39C, 1.7.99 +0x3BA
untested): `ShadowMask::DrawWorldMasks(clear)` clears kSHADOW_MASK through `Skeleton::ClearColour`, builds the draws with the
snapshot's main camera (near, far) and the frame camera's posAdjust, the observed starting modes (`Modes::WorldStart`), submits,
and returns every caster's maps through CS's port (`Shadows::Native::ReturnShadowmaps`); `LeaveMaskState` and the current camera
global follow. A batch the build refuses is only cleared and logged once per reason (in native the engine fallback drew nothing
either, its draws being dropped).

Terrain Blending's thunk no longer calls SetCameraData in native (nothing in native's depth pass reads it).

Not made in native any more: StartAccumulating x(1 + focus targets), ClearItemsList, RenderFirstPersonView_End, AE 106569, the
first-person cull and SubmitAccumulator, AE 107150, the depth range and node hide writes, the masks' EndTechnique,
SetCurrentDepthStencilTarget/SetCurrentRenderTarget, the state flush, SetClearColor/RestoreClearColors, SetCameraData x2-3, the
engine's ReturnShadowmaps vfunc.

## Detector (engine render calls per native world frame)

The sites inside RenderDepth (StartAccumulating #1 and #2, AE 106569, RenderFirstPersonViewEnd, BSCullingProcess::Process,
AE 76551/76552) count 0 in every run. `Main::RenderShadowMasks (in Main::RenderPlayerView)` now counts as a CS-hooked skeleton call
(1.00, "hooked"), like Main::RenderDepth and Main::RenderWorld: the call site stays until CS drives RenderPlayerView itself
(phase 7, decision 70); the engine function is not entered.

| Config | Before | After |
|---|---|---|
| pov (third then first person) | 29.2 (`-a-base-pov`) | 22.1 |
| native-vulkan+quick | 33 (snap3-accum) | 30.7 |
| native-d3d12-quick | - | 30.2 |
| native-interior | 20 (snap3-accum) | 16 |
| shadows-blend | 24 | 18.5 |
| window-resize-stress-taa | 24 | 19 |
| cadence-drop-cells | 27 | 24.4 |
| screen-blood | 27 | 24 |

Live engine reads per world frame: quick 695, interior 576 (unchanged within scene noise; the masks' settings globals and the
shadow lights are still read at record time).

## Runs (stage `s7-depth`, Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-vulkan + scratch pov config, base build | `s7-depth-a-base-pov` | passed (29.2) |
| same, observe | `-b-observe-pov` | passed; numbers above |
| same, switched | `-c-switch-pov` | passed (22.1); shadows and first person drawn |
| guest+quick | `-d-guest-quick` | passed |
| native-vulkan+quick | `-d-nvk-quick` | passed |
| native-d3d12-quick | `-d-d3d12-quick` | passed |
| native-vulkan+native-interior | `-d-interior` | failed: the submission stalled a minute after the coc (GraphHost "epoch has no published copy"), no shot |
| native-vulkan+native-interior (reruns) | `-e-interior2..4` | all passed (16 render calls); the stall did not recur in three runs |
| native-vulkan+shadows-blend | `-d-shadows-blend` | passed |
| native-vulkan+window-resize-stress-taa | `-d-wrs-taa` | passed (12 resizes) |
| native-vulkan+cadence-drop-cells | `-d-drop-cells` | passed |
| native-vulkan+screen-blood | `-d-blood` | passed |
| pov, final build | `-e-pov` | passed (22) |

SE was not run (new addresses are SE/AE pairs checked in both disassemblies: RenderShadowMasks' call at +0x39C, the scene's
shadows byte SE 513204 / AE 390933 read by both functions).

## Decided by the agent, to confirm

- DP-a. The focus targets' stencil marks are not drawn in native: the only reader is the mask shader's focus branch, which neither
  the engine's draw under DXVK (stencil view reads 0) nor CS's mask program (stencil value 0) takes. Focus shadows therefore stay
  off in the masks, as before.
- DP-b. RenderDepth's first-person block is not made: DCLF draws first person from its own camera (taken at the Z-prepass point,
  before the block) and rows; AE 107123 in RenderPlayerView already set the camera, node shift and shader state the block redoes
  with the same values; the cull is render-only (decision 95). Only its draw point (hidden bits, depth range) remains.
- DP-c. CS writes the shadow state the two functions leave (`LeaveDepthState`, `LeaveMaskState`) and the current camera global,
  for the engine code still after them (VolumetricLighting::Render, RenderFirstPersonView, RenderPostProcess), as AC-a did for
  RenderWorld. They go once that code is CS's.
- DP-d. The masks' world batch takes its camera from the snapshot (near, far) and the frame camera (posAdjust), and its starting
  modes as constants (observed). The masks' settings globals and the shadow lights are still read at record time.
- DP-e. A refused native world batch is only cleared and logged (not fatal): the first world frame of a load meets a light whose
  map has no target yet, once per load; the engine fallback drew nothing in native either.
- DP-f. Terrain Blending's SetCameraData before the depth pass is skipped in native.

## Proposed decision-log entries

- **Native's RenderDepth and RenderShadowMasks call no engine function** (refines 127): CS drives both from its frame. RenderDepth:
  the main depth clear and its copy through the skeleton, DCLF's Z-prepass point and first person's draw point; no depth
  accumulator, focus stencil marks or first-person block. RenderShadowMasks: the world masks cleared and drawn by DCLF's family
  from the snapshot camera, the maps returned by CS's port. Both leave the shadow state the engine code after them reads.
- **First person in native is DCLF's alone:** the engine's first-person cull, camera shift and submit inside RenderDepth are not
  made (their net effect was nil or render-only).
- **The focus shadows' stencil is not drawn** (no reader in native or under DXVK).

## Open items

- The interior submission stall seen once in four runs (`-d-interior`); it did not recur. If it does, it needs a hang capture; the
  "epoch has no published copy" line matches the known copy-queue sealed stall more than anything in this change.
- RenderFirstPersonView, VolumetricLighting::Render and RenderPostProcess still read the engine state CS writes for them.
- The masks' settings globals (filter, Poisson radius, biases, map resolution) and the shadow lights are record-time reads.
