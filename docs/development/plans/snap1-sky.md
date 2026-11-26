# Snapshot roadmap phase 1: sky, atmosphere and lighting (lead `sky`)

Branch `snap1/sky` (base `org_integration` aba6c707d), stage `s1-sky`. Roadmap: [render-thread-snapshots.md](render-thread-snapshots.md),
contract: [snap0-contract.md](snap0-contract.md), detector: [snap0-detect.md](snap0-detect.md).

## What changed

Every feature below has a typed block (`Snapshot::BlockId`, the sky group's range after `TreeWind`), a `Capture(Snapshot::Builder&)`
registered in `PostPostLoad`, and record code that reads the block from `Snapshot::Current()`:

| Block | Captures | Read by |
|---|---|---|
| `SkySync::Block` | Sky Sync's own results (sky-local and world directions of sun, Masser, Secunda; the directional light blend) | `PhysicalSky::Reset` (sun and moon directions, disc body, blend) |
| `PhysicalSky::Block` | worldspace editor ID, screen size, camera root position, sun/moon billboard half tangents, the scene sun light's direction | `Reset`, `Prepass` (history key), `RenderVolumetricClouds` (cloud light direction) |
| `ProceduralSun::Block` | the sun quad's model radius | `GetCommonBufferData` |
| `ProceduralAurora::Block` | the vanilla aurora fade `OnSkyUpdate` read | `PackCommonBufferData`; the day count and pause come from the core fields (`gameDaysPassed`, `menus.paused`) |
| `CloudShadows::Block` | full sky, climate, the cloud layers' geometry addresses (keys only, never dereferenced) | `FindCloudLayer` (51 reads per frame before), `ReflectionsPrepass` |
| `Skylighting::Block` | full sky | `Prepass` |
| `TerrainShadows::Block` | the land worldspace's editor ID, the scene sun light's direction | `IsHeightMapReady`, `LoadHeightmap`, `UpdateShadow` |
| `InteriorSun::Block` | the TES interior cell and its sun flags | `EarlyPrepass` |
| `VolumetricLighting::Block` | screen size, interior cell and its sun flags | `EarlyPrepass` |
| `WetnessEffects::Block` | full sky, precipitation, rain intensity, wetness and puddle wetness of the two weathers | `GetCommonBufferData` |
| `ExponentialHeightFog::Block` | the sky's kHideSky flag | `ORGExponentialHeightFog` (its observation of the shadow inputs) |

Other changes:
- **frameCount out of predicates and commits.** `globals::state->frameCount` (moves at Present) is replaced by the render frame,
  `RenderGraph::FrameClock::BeginFrame()` (moves at Begin), in the `active()` predicates, commits and same-frame/consecutive-frame
  checks of ORGPhysicalSky, ORGVolumetricClouds, ORGSkylighting (and `Skylighting::BeginOcclusionFrame`), ORGViewAtmosphere,
  ORGViewAtmosphereReflection, ORGViewAtmosphereVolume, ORGExponentialHeightFog, and Procedural Aurora's once-a-frame pack. Both
  move once per render frame, so pairs of checks behave as before.
- **SkySync's INI reads.** The nine `fVolumetricLighting*:Display` settings are resolved at `DataLoaded` (as `fSunAlphaTransTime`),
  not in function-local statics. They are read live by the game-side colour hooks only.
- **SkySync's update is game-side.** `Sky_Update::thunk` (Sky::Update, the game's update) opens a `CaptureScope`: its work is game
  logic (decision 8) and its results reach the frame through `SkySync::Block`.
- `ORGSkylighting`'s unused `Inputs::esramShadows` (the graph imports the target by id) and its renderer read are gone.
- `ORGViewAtmosphere::CompositeAfterWater` takes kMAIN's description from CS's target set (`Targets::Desc`), not the renderer.
- Null checks of the renderer singleton before view identity lookups are gone (ORGVolumetricClouds, ORGExponentialHeightFog).

## Game-time integrators (proposed decision-log entries, for the director)

All step **once per new snapshot by `events.gameDelta`**, as Tree Wind does: a repeated snapshot holds them, a dropped one does not
lose time, and guest (one snapshot per render) steps exactly as before.

- **Lightning** (`SkyLightning::Update`): world state (strike timing, thunder). Once per snapshot.
- **Cloud wind** (`PhysicalSky::UpdateCloudWind`: offsets, development phase, storm time, shear): world state as weather motion, but
  visual-only in effect. Once per snapshot for now (decided by the agent, to confirm): render delta would change guest under
  slow motion and pauses, and phase 2 can move it to render delta.
- **Wetness rain clock** (`WetnessEffects::Reset`): visual animation of ripples. Once per snapshot (same reason; to confirm).
- **Storm**: steps through `volWind.stormTime`, so it follows the cloud wind.
- **Procedural Aurora's clock**: already wall-clock (visual), paused by the snapshot's `menus.paused`. Unchanged.

## Left in place, and why

- **Frame-buffer camera (bound state)**: `frameBufferCached` reads (CameraPosAdjust, ViewProj, Unjittered, DynamicResolutionParams1)
  in PhysicalSky, VolumetricClouds, ORGVolumetricClouds, Storm, VoxelCloud, Lightning, ViewAtmosphere, ORGViewAtmosphere,
  ExponentialHeightFog. Waiting on core's frame-camera port (core lead: not in phase 1).
- **Engine render calls and their outputs (phase 3)**: Skylighting's occlusion maps (`RenderOcclusionAtShadowMaps`,
  `RenderOcclusion`, `RenderSkyOcclusionMap`: the precipitation object, the renderer's depth table and the shadow/graphics state
  they save around engine render functions); Wetness's rain occlusion projection (`RainOcclusionProjection`: written by the
  engine's precipitation occlusion render in the same frame, so a capture would lag it a frame); Exponential Height Fog's engine
  local shadows (`BuildEngineLocalShadowData`: shadow light descriptors from the engine's shadow render); Rain Refraction's and
  Cloud Shadows' engine state flags (D3D11 draw hacks); Interior Sun's culling hook (player position, in the engine's caster cull);
  Lightning's ground pick (`TES::Pick`, not reached in these runs).
- **Engine target identity lookups** (`CloudEngineDepthOf`, ORGSkylighting's `EngineDepthOf`, ORGExponentialHeightFog's `SceneDepth`):
  they resolve a view the engine bound (Deferred's capture) to an engine target; the fallback for DXVK objects walks the renderer's
  depth table. They go with the bound views they resolve.
- **PhysicalSky's VS b6 and samplers, ViewAtmosphere's inherited SRVs**: the markers did not fire in native's world frame in any run
  (D3D11 paths); guest keeps them.
- **Skylighting's `frameCount % 4` quadrant**: phase 2 (render frame).
- **Temporal noise indices** (`ExponentialHeightFog` `frameCount % 8`, `(frameCount - i) & 1023`): render identity, phase 2. Moving them
  to FrameClock now would shift the noise phase in guest.
- **Settings UI reads** (PhysicalSky's worldspace helpers, SkySyncDebug, InteriorSun's slider): category Other.
- **Cross-thread writes**: `PhysicalSky::Reset` writes `SkySync::workingLightColors` (render side), read by Sky Sync's game-side
  update. Phase 7 needs it as a render-to-game value or a setting-derived value.

## Shared values to fold into core's block after the merge

Captured in my blocks now; `Snapshot::CoreFrame` (snap1/core) carries them: screen size (`Util::DisplaySize()`), the sun light
direction (`dirLightDirection`; PhysicalSky and TerrainShadows blocks), hideSky (ExponentialHeightFog block), sun/moon directions
(SkySync block feeds core's capture).

## Measurements (live-read detector, `CS_SNAPSHOT_DETECT=1`, exit table, Dev-Fast, autoloaded save)

Live reads per native Vulkan world frame at this group's sites (sum of sites per feature), before (base build aba6c707d) and after:

| Feature | quick: before | after | physical-sky: before | after |
|---|---|---|---|---|
| CloudShadows | 54.3 | 0 | 54.3 | 0 |
| TerrainShadows | 10.1 | 0.02 (capture-point: the Papyrus game-hour hook) | 10.3 | 0 |
| PhysicalSky (with VolumetricClouds, Lightning, Storm) | 14.1 | 0 | 19.8 | 2.7 (bound state: frame camera) |
| WetnessEffects | 6.9 | 4.2 (rain occlusion projection) | 6.2 | 4.0 |
| Skylighting | 6.6 | 4.7 (occlusion maps' engine calls) | 6.9 | 5.0 |
| ProceduralSun | 4.2 | 0 | 4.0 | 0 |
| ProceduralAurora | 4.0 | 0 | 4.0 | 0 |
| VolumetricLighting | 3.0 | 0 | 3.0 | 0 |
| ExponentialHeightFog | 3.0 | 3.0 (engine local shadows, frame camera) | 3.0 | 3.0 |
| ViewAtmosphere | 2.8 | 2.0 (bound state: frame camera) | 3.6 | 2.6 |
| SkySync | 2.0 | 0 | 0.6 | 0 |
| InteriorSun | 1.0 | 0 | 1.0 | 0 |

By category, this group's sites per native world frame:

| Run | record-time | bound-state | other |
|---|---|---|---|
| quick | 96.9 -> 10.8 | 2.9 -> 2.9 | 12.1 -> 0 |
| physical-sky | 98.3 -> 11.0 | 6.3 -> 6.3 | 12.2 -> 0 |
| clouds | 100.8 -> 11.0 | 9.1 -> 9.0 | 12.8 -> 0 |
| cloud-shadows | 67.7 -> 9.9 | 3.3 -> 3.2 | 12.0 -> 0 |
| view-atmosphere | 97.0 -> 12.7 | 4.6 -> 4.6 | 12.9 -> 0 |

The remaining record-time sites are all in "Left in place": Wetness's rain occlusion projection (4/frame), Skylighting's occlusion
map engine calls (5/frame), Exponential Height Fog's engine local shadows (2/frame) and its scene depth identity (0.2/frame, view
atmosphere only). The remaining bound-state sites are the frame-buffer camera.

## Runs (cs-test, stage s1-sky)

| Config | Label | Result |
|---|---|---|
| guest+quick | s1-sky-base-guest (base), s1-sky-a1-guest, s1-sky-a1-guest2 | passed |
| native-vulkan+quick | s1-sky-base-nvk (base), s1-sky-a1-nvk | passed |
| native-vulkan+physical-sky | s1-sky-base-psky (base), s1-sky-a1-psky | passed |
| native-vulkan+clouds | s1-sky-base-clouds (base), s1-sky-a2-clouds | passed |
| native-vulkan+cloud-shadows | s1-sky-base-cshadow (base), s1-sky-a2-cshadow, s1-sky-a2-cshadow-tgm | passed |
| native-vulkan+view-atmosphere | s1-sky-base-vatm (base), s1-sky-a2-vatm, s1-sky-a2-vatm-tgm | passed |
| guest+view-atmosphere | s1-sky-a2-gvatm | passed (the post-water composite ran: `Targets::Desc` gives kMAIN in guest) |
| native-d3d12-quick | s1-sky-a2-d3d12 | hang at the loading screen: the serving thread stuck in the D3D12 driver in `d_createCommittedTexture` for an engine UI texture, nothing of this branch on the stack |
| native-d3d12-quick | s1-sky-a2-d3d12b | passed |
| native-vulkan+cadence-repeat | s1-sky-a2-repeat | passed |

Guest shots: base against after differ by a mean channel difference of 0.72; two runs of the same after build differ by 0.71, so
the change is inside run-to-run noise. The autoloaded save has a hostile guard: the configs without `tgm` (cloud-shadows,
view-atmosphere) let the player reach low health or die, so their shots differ in content (a blown-out "atmosphere-off", black
interior and exterior shots after the coc). The reruns with a `tgm` first step (`--set steps=...`) are normal (atmosphere-off mean
luminance 27 against the base's 14, interior 10 against 11).

## Decisions by the agent, to confirm

- **One block per feature**, no shared sky block (the post lead asked): blocks stay owned by their feature. `SkySync::Block` carries
  Sky Sync's world directions for other features.
- **Sky Sync's Sky::Update hook opens a capture scope**: game logic on the game thread (decision 8), so its reads are not live reads.
- **Integrators step by game time once per snapshot** (above), including the visual-only cloud wind and rain clock, to keep guest
  unchanged; phase 2 may move the visual ones to render delta.
- **Rain occlusion projection stays a record-time read** (the engine writes it in the same frame's render half); capturing it would
  lag it a frame in guest.
- **Cloud layer geometry as address keys** in `CloudShadows::Block` (compared, never dereferenced), since the DCLF sky draws hand the
  engine pass to `ModifySky`.
- **frameCount to FrameClock** where it is identity (same frame, consecutive frame); left where it is a temporal noise index.

## Open items

- Fold the shared captures into `Snapshot::CoreFrame` once snap1/core is merged (screen size, sun light direction, hideSky).
- The frame-buffer camera port (core): then the bound-state sites above go.
- Phase 3: Skylighting's occlusion maps, the precipitation occlusion projection, Exponential Height Fog's engine local shadows,
  Interior Sun's caster cull hook, Rain Refraction's and Cloud Shadows' engine state flags, Lightning's ground pick.
- Phase 2: Skylighting's quadrant, Exponential Height Fog's temporal indices, the cloud wind and rain clock by render delta.
- Phase 7: `SkySync::workingLightColors` written by the render side, read by the game-side update.
