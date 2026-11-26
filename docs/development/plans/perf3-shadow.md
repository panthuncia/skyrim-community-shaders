# Round 3 lead `shadow`: native shadow maps

Branch `perf3/shadow` (CS), based on `org_integration` f6b0d91de. No ORG, BasicRHI, DXVK or SceneWriterFramework change.

## What changed

A shadow light whose views DCLF draws whole no longer runs its `Render`. The three `Render` vtable slots (0x0A of
BSShadowDirectionalLight, BSShadowFrustumLight, BSShadowParabolicLight, CommonLib VTABLE ids, so SE and AE) are hooked in
`Features/Shadows/Engine/ShadowMapsNative.cpp`. Inside `Main::RenderShadowMaps` (between DCLF's BeforeShadowMaps and
AfterShadowMaps) a light goes native when every view its `Render` would draw is a covered view, no covered accumulator kept a pass for
the engine this frame (new `PassCapture::CoveredKept`, filled in `KeepForEngine`), and no geometry group of its batch renderers holds a
pass. Otherwise the engine's `Render` runs as before.

The native path, ported from `Render` (AE 0x141511d60 / 0x14151aac0 / 0x14151bd10, SE 0x141324c30 / 0x14132d040 / 0x14132e4e0) and
`RenderShadowmap` (AE 0x1414f0cf0, SE 0x141305610):

1. captures each view first (`IndirectDraws::CaptureNativeShadowView`): the camera is its shadow source's, the rasterizer state its
   placement's, ParabolaParam a paraboloid's radius and side. No renderer state is read. If one view cannot be captured, the light's
   captures are dropped and the engine draws it;
2. writes the descriptors as `Render` does: cascade targets 2 (3 for the volumetric copy) and slices, focus target 4, slice
   `base + i` and the clear flag (spot focus also `isEnabled`), the second hemisphere's target, target -1 to 4 with the free mask's lowest
   slice; advances the light index;
3. releases the scene accumulate array, clears each clearing slice through `Skeleton::Sites::TakeDepthClear` (depth 1, the engine's
   view, D3D11 only if the skeleton does not take it);
4. writes each descriptor's light transform (`ShadowCameras::LightTransform`: the grown, top/bottom-swapped frustum, view with
   translation, projection, texture scale for orthographic non-paraboloid cameras);
5. leaves what the engine leaves: the last draw's camera state (State::SetCameraData from axes, AE 77504 / SE 75695, on the State object
   AE 411479 / SE 524998), the current camera (414580 / 527667) and accumulator (414600 / 527650), the render mode (SetRenderMode), the
   paraboloid side, radius -1.

`CaptureShadowView` now shares one body with the native capture. New addresses are in `ShadowEngineAddresses.h`, each an AE/SE pair
checked in Ghidra (both runtimes' functions decompiled; the SE functions are the same code). 1.7.104 uses the AE ids.

## Parity (temporary compare build, removed)

The native path ran first, then the light's state was put back and the engine's `Render` ran, and the two were compared: light index,
free mask, every descriptor field, light transforms, clears (view, flags, depth, stencil), engine pass draws, captured views (every
field, constants, planes), globals. Guest, river at 13:00, inventory, Dragonsreach:

- first run: views differed only because the harness left the native views in the list (the engine's then hit "already drew at");
- second run: 4201 of 4201 lights equal, 9867 views, 8154 clears, no engine pass drawn, light transforms bit-exact.

It also showed the engine's `Render` always moves posAdjust. Without step 5's camera state the river shot lost its depth of field
(mean difference 9.6); with it, river 1.4, inventory 1.3, Dragonsreach 2.6 against the engine build (its own run-to-run is 2.4).

## Measured (Dev-Fast, guest+quick frozen river; sun with 4 cascades, the volumetric copy and a focus view)

| | base | after (after2) |
|---|---|---|
| window | 2.781 ms | 2.631 ms (other leads ran games; mostly noise) |
| `Main::RenderShadowMaps` inclusive | 0.402 | 0.375 |
| engine self in it | 0.044 | 0.013 |
| `CS DCLF: shadow view capture` | 0.013 | gone |
| `CS.DCLF.ShadowMaps.Native` | - | 0.028 |
| `JobList::Finish` in it | 0.008 | 0.018 |

Net about 0.016 ms. The engine's `Render` overlapped the main scene's register jobs, so part of the saving now waits in that
`JobList::Finish`. Cutting that wait means the register jobs themselves.

## Tests (final build)

| Config | Label | Result |
|---|---|---|
| guest, river noon + inventory + Dragonsreach | p3-shadow-n4-guest-shadows | passed, shots match the engine build |
| native-vulkan, same steps | p3-shadow-f-nvk-shadows | passed, sun and Dragonsreach pillar shadows present |
| guest-interior | p3-shadow-f-guest-interior | passed |
| guest+quick perf | p3-shadow-after1, -after2 | captured |

`p3-shadow-n1-guest-shadows` hung in `StopFrameProduction` (graph install, lead `install`'s item); the rerun passed.
`p3-shadow-n3` loaded another save in its lab slot (no river shot) and is not used. Native D3D12 not run (no ORG or BasicRHI change).

## Scaleform HUD and anchor clients (item 2): not changed

- `MenuManager::DrawInterface` self 0.065-0.072 ms is Scaleform's own CPU work. Nothing CS does there is separable; only a native HUD
  would remove it, which is not a contained step.
- `CS.Anchors.ClientCommit` 0.135 ms is about 45 commits a frame at 2-3 us each, spread over every anchor (PrepassPasses 7 commits
  0.018, EarlyPrepasses 6 commits 0.015, the tonemap call site 0.017, the Scaleform client 0.014, sky faces 0.014). No client dominates.
  Each commit also does `GraphFrames::NoteGraph` (a mutex and a string map lookup; test support, compiled out of the Release profile).
  Plan: tag commits by client in a capture (Tracy csvexport drops zone text, so a per-client zone name is needed) before cutting.
- Item 3 (ORG per-epoch remainder) not started.

## Decided by the agent, to confirm

- The native path calls the engine's State::SetCameraData (from axes) once per light to leave the camera state the engine leaves. Without
  it the world's depth of field changed. Reading that state at EarlyPrepass is the real dependency; once the EarlyPrepass work reads the
  main camera itself, this call can go.
- The renderer viewport `UpdateViewPort` would leave (the last camera's port) is not reproduced; nothing reads it before the next bind.
- A light whose views are partly covered, or which kept an engine pass, still runs the engine's `Render` (a correctness fallback, no switch).
- SE takes the same path. Untested on SE at runtime (the game folder is AE 1.6.1170).
