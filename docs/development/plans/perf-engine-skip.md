# Render thread: engine work the native path makes redundant (lead engine-skip)

Item 5 of [render-thread-window.md](render-thread-window.md), without the image-space chain (lead post-native). Branch
`perf/engine-skip`. All timings are Dev-Fast (/Od CS code, optimized engine), guest mode, the frozen river scene, one 6 s Tracy
capture each (`trun.sh guest+quick <label> --set env.CS_ENGINE_ZONES=1`).

## Zones one level deeper

`src/EngineZones.cpp` now wraps 262 call sites (259 written; three in `Main::RenderWaterEffects` hold another hook's patch). New
parents: `TESWaterSystem::Update` (AE 32174), `TESWaterReflections::Update` (32160), `BSCubeMapCamera::RenderFaces` (107564),
the accumulator's pre- and post-resolve renders (106583, 106584) and `RenderBatches` (106609), `BSCullingProcess::Process`
(106921), `CalculateActiveShadowCasterLights` (107137), `Main::RenderShadowMasks` (107140), `Sun::SubmitOcclusionQuery`
(26353) and `ImageSpace::RenderSSAO` (107245). Generated with the perf-tools `sites.py` and a copy of `gentable.py` with the
new names and skips; the copy lives in the lead's scratchpad (`engine-skip/ez/gen.py`, `sites.tsv`).

`StartAccumulating` and `FinishAccumulatingPostResolveDepth` reach the work through accumulator vfuncs (0x150, 0x158), so their
direct calls say nothing; the zones go in the vfuncs' mode-0 bodies (106583, 106584) instead.

## Where the engine's self time is (baseline, base3)

Window 7.03 ms (more zones than the director's r1 capture, so 0.5 ms more profiler cost); engine self 1.50 ms.

What the old top items were made of:

| Old zone (self) | Split |
|---|---|
| AE 32174 (0.24) | The water reflection's two faces a frame. `TESWaterReflections::Update` self 0.04; per face: the cull 0.025 (the sky root only: DCLF skips the LOD and tree roots), `SubmitAccumulator` (the sky batch and cloud group, both DCLF's native sky builds behind CS thunks) and `SetFace` (DCLF's face camera hook). The rest of 32174's inclusive time is CS: `Deferred::ReflectionsPrepasses` 0.33 (Tracy-inflated feature loops, plus the ReflectionsPrepass anchor). |
| `StartAccumulating` (0.17) | Mostly CS hook code at the batch call sites that `RenderBatches` zones now wrap: the sky batch thunk (#7, 0.04), the Deferred prepass hook (#1, 0.055), the cloud group thunk (`RenderGeometryGroup` #5, 0.01). The engine's own batch iteration is small (`BeginGroups` 0.016). AE 107146 (0.057) is an image-space step (ImageSpaceManager+0x1C0), post-native's. |
| `FinishAccumulatingPostResolveDepth` (0.06) | AE 107285 (0.025) is the temporal AA setup (image space); the rest are DCLF's water and forward epochs behind thunks. |
| `ImageSpace::RenderSSAO` (0.06) | Image space (post-native). |

What DCLF and ORG still consume, so it stays:

- The face render: DCLF's reflection epoch needs the face cameras (`SetFace` hook) and the sky family builds the faces' sky from
  the passes the face cull registers. Skipping the face render needs the sky's face passes computed from `Sky` itself and DCLF's
  own face cameras: a port, not a skip.
- The scene-list culling jobs (`CS.DCLF.ListJobsWait`): DCLF's PrimaryCull stands in for owned entries and consumes the rest.
- `Deferred::ReflectionsPrepasses`: Cloud Shadows queues its occlusion copy there and the ReflectionsPrepass anchor runs ORG
  clients (Cloud Shadows, View Atmosphere).
- `MenuManager::DrawInterface` (Scaleform), and `AE 106337` (BSShaderManager state per light, game data).

So the engine CPU left in this lead's scope is fragmented: no single redundant block above 0.05 ms. The skips below are the ones
that are real ports.

## Done: the sun query without the engine (sky step 4)

Commit `94f08efb6`. In native sky mode (`CS_DCLF_SKY_NATIVE=2`, the default) the hook on the engine's sun query draw (AE
106604, SE 99958) no longer calls the engine. It keeps the query entry's bookkeeping as the engine does, brackets the ring
entry, sets the query camera, and builds the SunOcclude draw with `Native::BuildSun` (details in
[dclf-native-sky.md](dclf-native-sky.md) step 4). New SE/AE address: the BSSkyShader singleton (AE 415143, SE 528198); the
query entry functions are SE 75565/75567/75568, AE 77366/77368/77369.

- Parity: observe mode (`CS_DCLF_SKY_NATIVE=1`) compares the native draw with the latched one. The first runs found two
  differences, both fixed: the eye comes from the shader manager's current accumulator (the screen splatter's), and SunOcclude's
  PParams.y is written as zero (its vanilla pixel shader declares none). The third observe run (river noon, river night,
  Dragonsreach) reported no difference.
- Cost (engine zones, guest): `Sun::SubmitOcclusionQuery` inclusive 0.040 -> 0.028 ms; the engine's 106604 is no longer
  called (its zone's 0.016 left is CS's bookkeeping, `SunShape`'s two GetDesc calls and the query hooks); the latch path's
  `Family::DrawTriShape` 0.011 is gone, `BuildSun` is 0.006. Whole window 7.03 -> 7.01 ms (noise is about 0.1).

## Not done, and why

- **Reflection faces** (the biggest engine block, about 0.15 ms of engine code over two faces): DCLF consumes the face cameras
  and the sky family's face passes. Skipping needs the faces' sky computed from the `Sky` object list rather than the face
  cull's registrations, and DCLF computing the cube cameras itself. A port of its own (decided by the agent, to confirm: not
  started here).
- **World accumulation**: what is left in `StartAccumulating` is CS hook code at the batch call sites and image-space steps,
  not engine accumulation DCLF has claimed. Nothing to skip.
- **Sky step 5 (delete the latch path)**: not done. Observe mode is the parity tool for the sky and now the sun, and the plan
  wants observe coverage of rain, storms, the aurora and night faces first (decided by the agent, to confirm).
- **Underwater mask clear in native mode** (handoff item 2): traced, not fixed. The engine's clears that the skeleton leaves
  on D3D11 (logged "stays on D3D11 (not a view the skeleton clears)") are rt12 `kUNDERWATER_MASK`, cleared from
  `ImageSpaceEffectDepthOfField::Render` (AE 0x1414e4470) inside `RenderEndOfFrameEffects` (the underwater DOF step), and rt88
  `kRAW_WATER`, cleared from AE 107285 (the water blend setup, 0x1414da1d0) inside the post-resolve render. Both belong to
  image-space families (the DOF slot effect and the water blend, family 6). Lead post-native confirmed both are outside its
  milestone and lists them as open items. No skeleton entries were added.
- `CS.DCLF.ListJobsWait`: the scene-list culling jobs stay (DCLF's PrimaryCull consumes them). Lead dclf-inputs looks at
  the stand-in's cost.

## Test runs

| Config | Label | Result |
|---|---|---|
| guest + scenes (river, river night, Dragonsreach) | p-engine-skip-sc-base-g | passed (baseline build) |
| guest + scenes, `CS_DCLF_SKY_NATIVE=1` | p-engine-skip-sun-obs, -obs2, -obs3 | passed; obs3 no differences |
| guest + scenes | p-engine-skip-sc-sun-g | passed; shots within run-to-run variance of the baseline |
| native-vulkan + scenes, `CS_DCLF_SKY_NATIVE=1` | p-engine-skip-sc-obs-nv | passed |
| native-vulkan + scenes | p-engine-skip-sc-sun-nv | passed; shots match the observe run's (night 1% of pixels > 1, mean 0.12) |
| native-d3d12 + scenes | p-engine-skip-sc-sun-nd | passed; the dropped engine draw errors are pre-existing (other leads' D3D12 runs show them) |
| guest+quick, native-vulkan+quick, native-d3d12-quick | p-engine-skip-final-* | all passed |
| guest+quick (Tracy) | p-engine-skip-base3, p-engine-skip-sun1 | captures; 'start' shot not written is trun.sh's step override |

Shot comparison: two runs of the same build differ as much as base vs after (river: 57% vs 60% of pixels by > 1 from the
water's motion; Dragonsreach 8% vs 17%; night 4% vs 2%, mean diff 0.25 vs 0.29), so the shots show no change beyond the
run-to-run noise. The sun's glare only depends on the query when the sun is on screen; the draw's field-by-field parity is the
stronger check.
