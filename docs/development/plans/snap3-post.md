# snap3-post: native RenderPostProcess and Volumetric Lighting without the engine (phase 3)

Lead: `post`, branch `snap3/post` (base `org_integration` 175c008da), stage `s7-post`. Phase 3 of
[render-thread-snapshots.md](render-thread-snapshots.md); decisions 8, 109, 122-127 of [render-thread-decisions.md](render-thread-decisions.md);
follows `snap3-accum.md` section 2 and its AC-a..c.

| Item | State |
|---|---|
| 1. RenderPostProcess not called; CS calls its points in order | done |
| 2. VolumetricLighting::Render (with SetCameraData #2, AE 107121 and the descriptor call) | done: a CS port of the VL render's CPU work |
| 3. Decision 124's writes | the render mode and the reflections' camera retired; TAA's form is CS state; `LeaveResolveState` stays (the engine's first-person view still reads it) |

## 1. RenderPostProcess (SE 100430, AE 107148)

Native never enters the engine's body. PostChain's detour of the function now calls `NativePost::RunNativeBody` in native (from
`ImageSpace::WorldPostProcess`), whatever the post group's state (each step's family decides itself whether CS draws it; only another
mod hooking the family brings back the engine's body). The order is the engine's:

1. `ORGVolumetricLighting::RenderNative` (VL's render, item 2: where Main::Draw ran it, before RenderPostProcess's reads);
2. `NativePost::ReadB` (refraction, VL activity, colour and parity from CS's VL in native, the camera from the snapshot);
3. the current camera (414580) written: native's shadow mask, effect constants and shadow code still read it next frame;
4. VL's apply and blur (`ORGVolumetricLighting::ApplyNative`: the graph run, no engine function);
5. the lens flare gather (AE 106988, still the engine's: CS takes its draws);
6. the pre-tonemap composites and the end-of-frame effects through their call site (Upscaling's and HDR's hooks run in order).

Not done any more in native: the TLS task tag (decision 109's rule), the four colour targets and the depth target set to none, the
flush (it was already gone), the depth of field's near/far copy into ImageSpaceManager +0x28's effect (the native rows read the
camera's planes at ReadB, never the effect), the camera port save and restore (nothing in CS's points writes the port).
Guest keeps `RunBody` unchanged.

The detector's row `Main::RenderPostProcess (in Main::RenderPlayerView)` is removed: like RenderWorld's call (never listed), the
call goes to CS's driver in native, and the sites inside RenderPostProcess (ApplyState, the VL descriptor, apply and blur, the lens
flares, the composites, the end-of-frame effects) count the engine's body wherever it still runs. All of them read 0 in native.

## 2. Volumetric Lighting: the vanilla path as a CS pass

Decision: **port the VL render's CPU work; keep both the vanilla and the feature path.** "The CS feature as the only path" would not
remove anything: the feature's ORG path uses the same engine CPU work (the generate's 0x180-byte constant block, the descriptor, the
activity test), only with its own shaders and volume size. The port serves both. Making the feature the only path would also change
what users who turned it off see. (Decided by the agent, to confirm.)

What the VL render (AE 107023, SE 100306 with its parts 0x1412dcce0 / 0x1412dc510) did, and where it comes from now:

| Engine | Native |
|---|---|
| Activity: off while DrawWorld +0x51 and INI 391215 are set, or 415052 is set; writes the apply's +0x8, the blur's +0x40, +0x190 +0x18 | game side: `Block::active`; nothing written |
| Copies the weather's render data (414627) into the descriptor while 414914 is set | game side: CS's own descriptor copy in the block |
| Noise volume creation | already the graph's |
| Constants (0x1414c57a0): view-projection and inverse (SetCameraData #2's), the two cascades' 3x4 with the eye, the splits, the INI flag, the sun direction, the volume size, the noise scroll plus the eye, the eye and the counter, the density settings | render side `GenerateRows`: the snapshot's main camera (unjittered view-projection, frustum, position), the cascades and splits from the sun shadow light as native's shadow pass left them (ShadowMaskNative's precedent); game side: sun direction, flag, density settings |
| Noise scroll: per VL render, `delta x density` times the wind's sin/cos and a random factor per axis | game side, once per game update (cadence-safe); CS's own random generator (the engine's sequence is not observable) |
| The descriptor's +0x88 counter (row 22 w, the target parity) | render side, CS's counter, one step per render (the TAA parity's rule) |
| The apply's +0x9 "no temporal blend" flag, cleared by the apply | game side: taken in the capture as a count; the first render after it applies it |
| VL colour (107017) | `NativeColour` from the block (the sun shadow light's diffuse, captured) |

Main::Draw's VL block (AE/SE +0x2F1 to +0x33A: the enable test, SetCameraData #2, AE 107121, the descriptor, the render) is jumped
over in native (`SideEffects::InstallHooks`, bytes checked, fatal on a mismatch), as the VL-off path does. SetCameraData #3 later
sets the same camera. `PostChain::VolumetricParity` and the description's VL activity read CS's VL in native.

### Observe (`s7-post-a-obs-interior`, native-interior, 600 VL renders)

CS's rows against the engine's block at every VL render: rows 0-20 and 22 equal (activity 0 differences, view-projection, inverse,
cascades, splits, sun direction, size within 1e-4); row 21 xyz not compared (the random scroll); one frame differed in the descriptor,
row 21 w and row 23 (the weather's render data reached the engine's descriptor one game update before CS's block: the render half
copies it after the capture). Accepted, as AC-e's far plane.

## 3. Decision 124's engine writes

- **The render mode (411475 = 7):** readers are the engine's RenderSSAO and BSEffectShader setup (AE 107522/107524), which native
  does not run; it was never 7 at RenderWorld's entry anyway (another writer, 82084/82085, sets it every frame). Not written.
- **The reflections' camera (415299):** Main::RenderScene (36558) writes the same camera before Main::Draw (0 differences over 1,800
  renders). Its one reader is game side: the radial blur modifier's projection (AE 107753 via 105691, ImageSpaceModifierInstanceRB).
  RenderScene's write keeps it; native's RenderWorld no longer writes it. Open for phase 7: RenderScene's write is the skeleton's.
- **TAA's form (+0x1f0 +0x2c):** CS state in native (`WorldPass::TaaForm`): 1 at the water mask point, 0 at TAA's step; the water
  blend test, the end-of-frame reads and the description read it. The engine object is not written in native. Its +0x38 water flag is
  still read.
- **`LeaveResolveState`:** stays. CS's RenderPostProcess reads none of it, but the engine's RenderFirstPersonView (AE 107129) still
  runs in native and flushes the shadow state (SubmitAccumulator with RT0/RT1 as RenderWorld left them). It goes with the first-person
  view's port.

## Detector

Engine render calls per native world frame: interior 20 (snap3-accum) -> 19 (observe build, RenderPostProcess row gone) -> 18
(VL render gone); post-effects 23 -> 21; quick 27. Live engine reads per world frame: interior 558 -> 557, post-effects 562. VolumetricLighting::Render, its descriptor, AE 107121 and SetCameraData #2 read 0. Other numbers in the runs table.

## Runs (stage `s7-post`, Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-vulkan+native-interior, observe, detector | `s7-post-a-obs-interior` | passed (19 calls); parity above |
| native-vulkan+native-interior, detector | `s7-post-b-interior` | passed (18 calls); VL shots match the observe run but for the guard's pose |
| guest+quick | `s7-post-c-guest-quick` | passed |
| native-vulkan+quick, detector | `s7-post-c-native-vulkan-quick` | passed (27 calls; snap3-accum's quick read 33) |
| native-d3d12-quick | `s7-post-c-native-d3d12-quick` | passed |
| native-vulkan+post-effects, detector | `s7-post-c-native-vulkan-post-effects` | passed (21 calls, was 23); shots within the run-to-run floor (dof-on 13.9 against snap3-accum's run; two runs of that base differ by 23.1) |
| native-vulkan+lens | `s7-post-c-native-vulkan-lens` | passed; lens 9.9, flat 22.6 against snap3-accum's (pose) |
| native-vulkan+upscalers | `s7-post-c-native-vulkan-upscalers` | passed; 9-24 against snap3-accum's (pose) |
| native-vulkan+window-resize-stress-taa | `s7-post-c-...` / `s7-post-d-...` | never loaded (stuck before data-loaded, before any code of this branch runs) / passed (12 resizes) |
| native-vulkan+cadence-drop-cells | `s7-post-c-...` / `s7-post-d-...` | failed (the Whiterun shot's readback was lost after 600 capture points) / passed, all three shots |

## Decided by the agent, to confirm

- PO-a. The VL render is ported, both paths kept (above).
- PO-b. The noise scroll steps once per game update with CS's own random generator.
- PO-c. The VL counter (target parity, row 22 w) is render side, one step per render; the no-temporal request is a game-side count.
- PO-d. The cascades and splits are read from the sun shadow light on the render thread (what native's shadow pass wrote), as the
  native shadow mask does.
- PO-e. The detector row of RenderPlayerView's RenderPostProcess call is removed (the RenderWorld call's precedent).
- PO-f. The current camera (414580) is still written by the driver (its native readers are other leads' items).
- PO-g. The lens flare gather (AE 106988) stays an engine call from CS's driver (its port is its own item).

## Proposed decision-log entries

- **Native's RenderPostProcess is CS's driver:** the engine's function is not entered; CS calls VL's render, the reads, VL's apply
  and blur, the lens flare gather, the composites and the end-of-frame effects in its order, with no flush and no target or depth of
  field writes. It writes only the current camera.
- **Volumetric Lighting's vanilla path is a CS pass:** the VL render's CPU work is ported (game side: activity, descriptor, noise
  scroll, sun; render side: constants from the snapshot camera and the shadow pass), for the vanilla and the feature path alike.
  Main::Draw's VL block is jumped over in native.
- **Decision 124 reduced:** the render mode and the reflections' camera are not written (RenderScene writes the camera the game
  reads); TAA's form is CS state; `LeaveResolveState` stays until the first-person view is CS's.

## Open items

- The lens flare gather (106988) as a native pass.
- The current camera's native readers (shadow mask, effect constants) should read the frame camera; then the driver writes nothing.
- `LeaveResolveState` with the first-person view's port.
- SE was not run. SE ids are pairs from the SE disassembly; SE's noise step (0x1412dc510) inlines its random calls and was not
  compared line by line.
- The vanilla VL path (feature off) was not run; the port is shared, only the volume size source differs (unchanged).
