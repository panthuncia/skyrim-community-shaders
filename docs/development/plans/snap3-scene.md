# snap3-scene: DCLF and the scene without the engine's render half

Lead: `scene`, branch `snap3/scene` (base `org_integration` 1c6773873), stage `s3-scene`. Phase 3 of
[render-thread-snapshots.md](render-thread-snapshots.md); decisions 8, 12-13, 33-48, 53 of
[render-thread-decisions.md](render-thread-decisions.md); starting point `snap3-lights.md` item 3.

## Plan and how far it got

The phase's items depend on each other more than the roadmap suggests (Ghidra, AE 1.6.1170):

- `Main::Draw` (RenderPlayerView, AE 36559) queues `DrawWorld_BuildSceneLists`; `CalculateAndDrawShadowCasterLights` (AE 107132)
  runs the main camera's list accumulation jobs (`FirstListAccumulationJob`/`ListAccumulationJob`, the main cull over the six scene
  lists, where DCLF's PrimaryCull stands in) and the shadow light selection; `Main::RenderShadowMaps` (AE 107133) runs the main
  camera's registration jobs (`BSAccumProcess::DoSceneListAccumRegisterJob` for the Z-prepass and main accumulators) while the
  shadow lights render. So the shadow port and the scene-list port share two engine functions (decision 48).
- Native still needs the registrations: the sky family builds its draws from the sky passes registered into the main accumulator
  (`SkyNative::BuildBatch`), the water family's native batches (wading displacement, refused water) likewise, and the CS stages are
  submitted at the accumulators' engine call sites (Sky, EngineDraws, the opaque points). Stopping the main cull first needs the sky
  and water draws from game data and CS-driven stage points.
- The engine-render detector counts a call site whenever the engine calls it, hooked or not. A port inside `Main::Draw` therefore
  removes the sites inside the replaced function's body, not the call of the function itself. Only replacing `Main::Draw` (CS
  driving the world frame) removes its ~20 sites.

Milestones:

| # | Milestone | State |
|---|---|---|
| M1 | Shadow casters: ports of the shadow lights' camera updates and bookkeeping (observe, then switch); 107132's and 107133's shadow work native; every shadow light native inside the shadow maps | done |
| M2 | Scene lists: sky and water draws from game data, the main cull and registration jobs not run, S8 for the decision 46 roots | not started |
| M3 | Item 4 ports (cube cameras, precipitation, screen blood, VL vanilla path) | not started |

## M1: native shadow casters

### The ports (`Features/Shadows/Native/`)

Each is a drop-in for the engine function it names: same inputs, the same writes in place, the same result.

- `SunCamera.{h,cpp}`: `BSShadowDirectionalLight::UpdateCamera` (AE 0x141512230, SE 0x1413251c0) with its helpers: the sun
  direction smoothing (0x1415148c0), the cull plane fit (0x141514c10), the convex hull (0x140e19490), WorldPtToScreen, the plane
  helpers. Writes the sun vector, splits, sunUpdate, the full-frustum camera and its processes' custom planes, each cascade's camera,
  texel snap, clip planes, port, unitsPerTexel, process planes and accumulator.
- `LocalCameras.{h,cpp}`: `BSShadowFrustumLight::UpdateCamera` (0x14151ac50), `BSShadowParabolicLight::UpdateCamera` (0x14151b620),
  the focus cameras (`BSShadowDirectionalLight::sub`, 0x1414f0480), the lens flare queue (0x1414cec70), the sphere projection and the
  light's scissor rectangle (0x140d2bab0), ReturnShadowmaps, the release of a light not kept, Accumulate's bookkeeping without the
  cull (frustum 0x14151aa60, paraboloid 0x14151b960, directional 0x141511c80), and the focus accumulators without registration
  (0x1414f0b90).
- Still called through the engine (object creation and scene-graph transforms, not rendering): memory allocation, the
  `BSShaderAccumulator` and `NiCamera` constructors (first use), `NiAVObject::Update`/`UpdateWorldData` on the lights' own cameras, the
  lens flare list's growth, and the portal entry's `ClearVisibility` (it takes the portal graph's lock).
- The port agents found one real SE/AE difference (the scissor rectangle's rows) and that CommonLib's `unitsPerTexel` is a float.

### Parity (observe build, removed)

A temporary harness wrapped the engine's calls (vtable slot 0x10 of the three light classes, the focus cameras' entry, slot 9 of
the local lights): it saved every range the port may write, ran the port, kept its bytes, restored, ran the engine, and compared
word by word (1e-4 relative for floats).

| Run | Result |
|---|---|
| `s3-scene-par1-interior` (Dragonsreach) | spot and point cameras: 0 differences in every 600 calls; accumulate: only the paraboloid's back accumulator (DCLF's empty cull never set it; the port does as the engine does) |
| `s3-scene-par2/3/4-nvk` (river save) | sun: the full-frustum camera, cascade 0 and every split identical; cascade 1's frustum right edge 88 units wider in the port on most frames (port -697.7, engine -608.9), with the custom cull planes that follow from it |

The cascade 1 difference was traced through the disassembly (corners, basis, the per-cascade bounds reset, the projection origin):
the port's eight slice corners and projections agree with the engine's code as read, and its bound is the slice's true extent; the
engine's tighter edge was not explained. Decided by the agent, to confirm: accept it. The port's cascade is a superset of the
engine's (no lost casters), and cascade 0 covers the overlap.

### The switch

- **Selection** (`NativeCasters.cpp` `SelectCasters`, in place of 107132 at Main::Draw's call, `ShadowCasterPoints.cpp`): resets the
  counts and `shadowLightsAccum`, the sun's camera (port), DCLF's sun views, its focus cameras and views, its keep, mask index and
  Accumulate bookkeeping, then `LightSelection::SelectNative`: DCLF's existing port of `CalculateActiveShadowCasterLights` now also
  calls the camera ports (their result is the visibility), keeps each light (slot, `ShadowCasterKept`, mask index, scissor rectangle,
  Accumulate bookkeeping, lens flare), makes the focus host's cameras and accumulators, and releases lights not kept. Stage 5 stays
  the game-side producer's (S1).
- **Not run in native:** the sun's full-frustum and cascade culls (`FUN_141511f30`), every light's cull and pass registration, the
  focus targets' registration, the attach walk `FUN_1414a2530` (decided by the agent, to confirm: it only builds geometry light lists
  for engine draws, which native never makes; DCLF mirrors its portal side with `PortalViews::CommitLightWalk`).
- **Shadow maps** (`RenderShadowMaps`, in place of 107133 at Deferred's hook): each light in `shadowLightsAccum` goes through
  `Engine::RenderLightNatively` (ShadowMapsNative's capture and bookkeeping). A light DCLF cannot draw casts no shadow that frame
  (counted in the existing `native shadow maps over 600 frames` line); the engine's Render is never called from here. Seen only in the
  first ~10 frames after a load, before the shadow epoch is recorded.
- **Engine scene work still run from here (M2's):** `Main::AfterFullFrustum` (DCLF's PrimaryCull prepares there), the portal
  records, the list accumulation jobs and their Finish, the registration jobs and their Finish, and the Z-prepass groups' prepare
  (AE 106613). The Finish calls go through the engine call sites' targets, so DCLF's stand-in hooks and the detector see them.
- Guest and D3D11 are unchanged (`Native::Replaces()` is native only; `LightSelection::Select(false)` is the old loop).
- `SunViews::Update` and `FocusViews::Update` are public now (native calls them after its ports).

### Detector (native Vulkan quick, river save, per world frame)

| Site | Before (`s3-lights-f-nvk-quick`) | After (`s3-scene-m1-native-vulkan-quick`) |
|---|---|---|
| `BSShadowDirectionalLight::Select` (the focus cameras, in 107132) | 1.01 | 0 |
| `CalculateActiveShadowCasterLights` (in 107132) | 1.01 | 0 |
| helpers in 107132 (106367, 106365, 76815/76816/76819 x6, 16341 x6) | ~20 | 0 |
| `Main::CalculateAndDrawShadowCasterLights`, `Main::RenderShadowMaps` (Main::Draw's calls, now CS's) | 1.01 each | 1.01 each (hooked) |
| `JobList::Finish` in 107132 and in 107133 (scene-list jobs) | 1.01 each | 1.01 each |
| total | 73.7 | 71 (guest 70, D3D12 70) |

The functions' bodies (the engine's shadow selection, camera setup, culls, registrations and Render) are no longer entered in
native; their call sites still count until Main::Draw is CS's.

## Runs (stage `s3-scene`, Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-interior (parity) | `s3-scene-par1-interior` | passed |
| native-vulkan+quick (parity) | `s3-scene-par2-nvk`, `par3`, `par4` | passed |
| native-vulkan+quick (switch, first builds) | `s3-scene-sw1/2/3-nvk` | passed; sw2/sw3 found the first ~10 frames' fallbacks |
| guest+quick | `s3-scene-m1-guest-quick` | passed |
| native-vulkan+quick, detector | `s3-scene-m1-native-vulkan-quick` | passed |
| native-d3d12-quick | `s3-scene-m1-native-d3d12-quick` | passed |
| native-vulkan+cadence-drop | `s3-scene-m1-native-vulkan-cadence-drop` | passed |
| native-vulkan+cadence-drop-cells | `s3-scene-m1-native-vulkan-cadence-drop-cells` | passed |
| native-vulkan+cadence-repeat | `s3-scene-m1-native-vulkan-cadence-repeat` | passed (decision 53's crash did not show; the engine's list culls still run, so it is not fixed) |
| native-vulkan+shadows-blend | `s3-scene-m1-native-vulkan-shadows-blend` | passed |
| native-vulkan+llf-local-shadows | `s3-scene-m1-native-vulkan-llf-local-shadows` | passed |
| native-interior | `s3-scene-m1-native-interior` | passed; 2,400 shadow lights native per 600 frames, none to the engine |
| native-vulkan+stealth-lights | `s3-scene-m1-native-vulkan-stealth-lights` | passed |
| native-tour (final build 9171c3a32) | `s3-scene-f-native-tour` | passed |
| cadence.py, native-vulkan+cadence-still (final build) | `s3-scene-f-cad-*` | all four runs passed; interior within noise (repeat 1.8, drop 0.4); exterior 16.0 / 15.3 against a floor of 2.6 this time, so "outside noise" by the rule, the same size as phase 2's accepted 15.2 / 17.5 (floor 8.7 then): wind and camera pose, shadows alike by eye |

The quick shots with `CS_SNAPSHOT_DETECT=1` are dark (the scene under exposure adaptation); the baseline run with the detector
(`s3-lights-f-nvk-quick`) and guest are equally dark, and the switch build without the detector (`s3-scene-sw3-nvk`) shows normal
sun shadows.

## Decisions (agent, to confirm)

- **SC-a.** The shadow lights' engine objects (descriptors, cameras, accumulators, `shadowLightsAccum`, the selection globals) stay
  the place native writes its shadow state, as ShadowMapsNative already did. Decision 47 (no game-side reader) holds; phase 6/7 moves
  them into render-owned memory.
- **SC-b.** Allocation, constructors and the lights' own camera transform updates are still the engine's functions (object
  creation, not rendering).
- **SC-c.** Cascade 1's wider edge in the sun port is kept (above).
- **SC-d.** The attach walk is not run in native (above).
- **SC-e.** A light whose views DCLF cannot draw casts no shadow for that frame in native, with no engine fallback.
- **SC-f.** S8 for the decision 46 roots waits for M2: the main cull still runs in native and steps their fades.

## Open items

- M2 and M3 (above). The next lead starts from the sky's registered passes (`SkyNative`), the water native batches, and CS-driven
  stage points for RenderWorld's accumulators.
- The engine Render calls outside `RenderShadowMaps` (UI3D's menu scenes, after a load) are phase 5's (menu scenes).
- SE is untested (every address is an SE/AE pair; the SE offsets in 100414/100415 come from the SE disassembly).
