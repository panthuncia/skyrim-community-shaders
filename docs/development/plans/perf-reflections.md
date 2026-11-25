# Render thread: the water reflection faces without the engine (lead reflections)

Item 5 of [render-thread-window.md](render-thread-window.md), the part [perf-engine-skip.md](perf-engine-skip.md) left: AE 32174
(`TESWaterSystem::Update`) renders two reflection cube faces a frame through `BSCubeMapCamera::RenderFaces` (vfunc 0x35, AE 107564,
SE 100780). Branch `perf/reflections`. Timings are Dev-Fast (/Od CS code, optimized engine), guest mode, the frozen river scene,
one 6 s Tracy capture each (`trun.sh guest+quick <label> --set env.CS_ENGINE_ZONES=1`).

## What the face render did, and who read it

Per face bit, `RenderFaces` sets the cube face as the target, applies state, orients the cube camera (`SetFace`, AE 107563), culls
the cube camera's roots into its accumulator (+0x1A0) and submits it with render mode 0 and flags 8. With DCLF owning the LOD
roots (the update's LOD add-roots are skipped), the cull registers only the sky root, so the face holds only the sky.

Its products and their readers:

- The face camera: DCLF's reflection epoch (`ReflectionView::FaceOriented`, from the orientation hook).
- The sky passes the cull registers: the sky family's face run (`SkyBatch`/`CloudGroup` thunks, `SkyDraws` at SkyReflections).
- The prepasses: CS's `Deferred` hook on vfunc 0x35 runs `ReflectionsPrepasses` (features, the ReflectionsPrepass anchor, View
  Atmosphere) and sets IsReflections around each call.
- Nothing else. Ghidra: the vfunc's only caller is `TESWaterReflections::Update` (32160) through a thunk at 0x1414ed4e0; `SetFace`
  is called only from `RenderFaces`. The cube camera's world transform (written by `SetFace`) has no other reader. The update's
  game logic (hiding the player and water shapes, the camera position, the face rotation counter, timings, the clear colour) stays:
  `TESWaterReflections::Update` still runs, only its vfunc 0x35 call is replaced.
- `WaterReflectionCubemapFix` and `UnifiedWater` hook `Main::RenderWaterEffects` (36560) and the water shader; neither depends on
  the face render (the fix clears the cube on a cell change and unbinds t3 in interiors).

## The port (CS_DCLF_REFLECTION_NATIVE, default 2)

In `ReflectionFaces::FaceRender`, a plain face (mode 0) whose update skipped every LOD root (DCLF draws them) is drawn without the
engine, in graph mode:

1. **Camera** (`NativeFace`): the cube camera's local translate (the update puts the eye there; the world transform follows only at
   `SetFace`), the face's rotation from `SetFace`'s table (D3D cube conventions in world axes, decoded from AE 0x1414ed500), the cube
   camera's frustum and port. `ShadowCameras::CameraMatrices` gained an overload from a transform, frustum and port.
2. **Sky** (`Sky::PrepareFace`, `Native::FacePasses`/`BuildFace`): the sky root is walked as the face cull does (hidden objects
   skipped, each object's bound tested against the face frustum's planes, `ShadowCameras::FrustumPlanes`; only the root is always
   drawn, as the update sets kAlwaysDraw on it). Each `BSSkyShaderProperty` geometry gets `GetRenderPasses`' pass id (AE 0x1414e6fd0;
   the sun glare has none in a face) and `FindTechniqueGroup`'s sub-group (SE 0x141308bb0). The batch is ordered by technique,
   sub-group and newest first (push-front); the cloud layers in registration order. The draws are built by the existing `BuildPass`
   with the face's frame: the face camera's view-projection, posAdjust = eye, previous posAdjust zero, alpha reference 128/255,
   stencil reference 255, the face viewport, render flags 8 (the cull mode is kept), starting state stencil mode 2, cull mode 2.
   Cloud Shadows' `ModifySky` takes the face flag instead of reading the shadow state.
3. `Deferred::BeginReflectionFace` (the prepasses and IsReflections, moved out of the hook so both paths share it), then
   `Sky::SubmitFace` (the header textures and published inputs the prepasses bound, Physical Sky's handles, Cloud Shadows' layer
   bookkeeping, `SkyDraws::Submit` at SkyReflections; the sky pass's first draw clears the face and its depth as before), then DCLF's
   face camera, then `EndReflectionFace`.
4. After the update's last face the cube camera's roots are released, as `RenderFaces` does with its clear-roots argument.

A face falls back to the engine's render when the sky family cannot take it (programs building, Cloud Shadows on D3D11, a
sky-object effect such as the aurora in view, a non-TriShape sky object); the reason is logged once.

Observe mode (`=1`): the engine renders; the native camera is compared with the engine's after `SetFace`, and at the engine's face
sky batch and cloud group the native pass lists, view-projection, posAdjusts, eye, alpha and stencil references, viewport, render
flags and starting state are compared with the registrations and the shadow state. `=0` is the engine path.

## The native-mode clears (item 2)

`kUNDERWATER_MASK` (rt12) and `kRAW_WATER` (rt88) now have skeleton clear passes (`Skeleton.cpp`, `kMoreColourTargets`):

- rt88 is cleared by the water blend (SE 100304, AE 107285) together with kWATER_1/2 when its pending-clear flag is set; it is
  hosted at the ImageSpace place like them.
- rt12 is cleared by `ImageSpaceEffectDepthOfField::Render` (AE 0x1414e4470) after the underwater DOF's masked composite, inside the
  post chain; it is hosted at the post chain's place (the post stage's last ImageSpace step), after the composite that reads it and
  before the next frame's start-stage mask draws.

No ImageSpace or PostChain file was touched. The log's "a clear of rt12/rt88 stays on D3D11" lines are gone; the world recipe lists
`rt12@post-chain` and `rt88@image-space`.

## Measured (Dev-Fast, guest, frozen river, Tracy)

Same build, back to back: `p-reflections-perf4e` (`CS_DCLF_REFLECTION_NATIVE=0`, the engine renders the faces) against
`p-reflections-perf4` (native, the default). Noise between captures is about 0.1 ms.

| | engine faces | native faces |
|---|---|---|
| Window | 6.71 ms | 6.40 ms |
| Engine self (all zones) | 1.56 | 1.25 |
| DCLF CPU | 0.64 | 0.75 |
| `Engine TESWaterSystem::Update` (the AE 32174 zone), inclusive | 0.80 | 0.63 |
| of it, engine zones' self | 0.34 | 0.06 |
| of it, `CS.DCLF.Reflection.NativeFaces` | - | 0.58 (prepasses 0.33, `BuildFace` 0.11, own 0.11) |

The pre-change baselines (`p-reflections-base3`, `-base4`) matched the engine-faces column (water update 0.78 and 0.80, its
engine self 0.32 and 0.33). `Deferred::ReflectionsPrepasses` (0.33, Tracy-inflated feature loops) is unchanged and is now the
largest part of the water update. The native faces' own 0.11 is the camera, the header and input reads, Cloud Shadows'
bookkeeping and the submit.

## Test runs

| Config | Label | Result |
|---|---|---|
| guest+quick (Tracy) | p-reflections-base3, base4 | baselines, unchanged branch |
| guest + scenes, `=1` (observe) | p-reflections-obs1 | stopped: a DCLF uncovered-epoch tap error after a Z-prepass rebuild 4 s into the load (not seen again in 12 later runs; one older nfg run had it). Its parity lines found the differences below |
| guest + scenes, `=1` | p-reflections-obs2 | passed; parity after the fixes: passes, cameras, view-projection, posAdjusts, eye, viewport, references, flags all equal; only the first face after a load had a different starting state (below) |
| guest + scenes | p-reflections-nat1, nat2 | passed; first native face logged, no engine fallbacks |
| guest + scenes, `=0` | p-reflections-eng2 | passed; same build, the engine's faces |
| native-vulkan + scenes | p-reflections-nv1, nv2 | passed |
| native-vulkan + scenes, `CS_ORG_SHOT_TARGET=12` / `=88` | p-reflections-t12, t12b, t12c, t88 | passed |
| native-vulkan, the river save with the player scaled down, target 12 | p-reflections-u12, uv | passed |
| native-d3d12 + scenes | p-reflections-nd1 | passed |
| guest+quick (Tracy) | p-reflections-perf2, perf4, perf4e | captures above |
| guest+quick, native-vulkan+quick, native-d3d12-quick | p-reflections-final-* | guest and D3D12 passed; native Vulkan failed once: its screenshot never saved ("capture 1 waits for a run that holds its copy"), not seen in any other run |
| the same, rebased on org_integration 3ed746144 | p-reflections-final2-* | all passed, each logs native faces |

Scenes: the river save (noon, then 23:00 clear), `coc Riverwood` and the handoff's `setpos` for underwater and above it, then
Dragonsreach. **`player.setpos` does not move the player in cs-test runs** (the shots after it show Riverwood's arrival point;
the sky-native config's river shots have the same problem), so the underwater scene was not reached; scaling the player down
(`setscale 0.1` in the river save) puts the camera at the water but still above it.

Shots: native against engine faces differ no more than two runs of the same mode (river 69% of pixels by more than 1 against
88-91% between same-mode runs, from the water's motion; river night 5% against 55%; Dragonsreach 44% against 20-43%).

Readbacks (native Vulkan): `kUNDERWATER_MASK` is empty in every scene (river, night, Riverwood, the scaled-down player at the
surface, Dragonsreach), so the mask was never drawn in these runs and the clear's effect could not be shown directly; the passes
are in the recipe and the engine's clears no longer reach D3D11. `kRAW_WATER` holds the river's water; in Dragonsreach it still
holds earlier water frames, because the engine clears it only when the water blend's pending-clear flag is set, which is the
same in guest mode.

## Decisions (by the agent, to confirm)

- **The switch stays** (`CS_DCLF_REFLECTION_NATIVE`, default 2): observe mode is the parity tool, as for the sky. Delete it with
  the sky's latch path.
- **The faces' starting state follows the engine's steady state.** The engine's state at a face's sky batch carries over from what
  ran before: after a load the first face had write mode 11 (later 1) and the cloud group stencil mode 0 (later 2). The native face
  always uses write 1 and stencil mode 2.
- **A sky-object effect in view (the aurora) sends that face to the engine** (logged once). In native mode the engine's face draws
  reach nothing, as before this change.
- **The sun glare is not drawn in faces**: its pass id is outside every range FinishAccumulating draws in a face.
- **rt12's clear hand-off**: lead post-slots makes the DoF slot native, so the engine's DoF tail stops running; its native DoF path
  calls `Skeleton::Sites::TakeColourClear` for rt12 under the engine's condition (the flag set and any DoF composite drawn), which reaches this branch's pass; it falls back to the D3D11 clear while the pass is missing.

## Open

- A scene with the camera crossing the water surface (the underwater mask drawn) to show the rt12 clear in a readback; it needs a
  way to move the player in cs-test runs.
- The native faces' own CPU (0.11 ms Dev-Fast): the header resolves and texture slots per face could be cached per frame.
