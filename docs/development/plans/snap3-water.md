# snap3-water: native's water effects, SetupSunAndSky and the precipitation occlusion maps without engine render calls

Lead: `water`, branch `snap3/water` (base `org_integration` 175c008da), stage `s7-water`. Phase 3 of
[render-thread-snapshots.md](render-thread-snapshots.md) ("Reflection cube cameras from the water data, not
TESWaterReflections::Update"); decisions 8, 37, 62, 64, 70 and 127 of [render-thread-decisions.md](render-thread-decisions.md).

## What changed

Code: `src/RenderGraph/Engine/Native/WaterEffects.{h,cpp}` (new), `ImageSpace/NativeStart.inl` (new), small edits in
`ReflectionFaces.cpp`, `StartSteps.inl`, `ImageSpace.{h,cpp}`, `EngineZones.cpp`, `Publication.cpp`, `DrawcallLimitFix.{h,cpp}`,
`Skylighting.cpp`, `Upscaling.cpp`, `WaterReflectionCubemapFix.{h,cpp}`. Native only; guest installs nothing new.

### 1. Main::RenderWaterEffects (SE 35561, AE 36560)

Main::Draw (RenderPlayerView, SE 35560 / AE 36559) no longer calls it in native: the `mov rcx, r12` before the call (+0x16F) becomes a
short jump over the call (bytes checked, mismatch fatal). Its game half runs in the snapshot capture on world snapshots, right after the
core block and before the feature captures (`WaterEffects::Capture`), as a port of the function's body:

- the early-out (map menu closed and water off, or a dry interior): the S20 resets, the shapes hidden (SE 31412 / AE 32219) and the
  underwater effects off (SE 31408 / AE 32215), as the engine does;
- `TESWaterSystem::Update` (SE 31383 / AE 32174) on the game thread with BSShaderManager::State +0x219 set around it, as the engine
  does. It is game logic (decision 8) except for three calls, redirected in native:
  - the water visibility test (SE 31398 / AE 32189, an engine cull using the main culling process) is CS's frustum test against the
    snapshot camera: the shape not hidden, and in an exterior only shapes under a BSMultiBoundNode (the engine's rule), tested by that
    node's bound. No portal, multibound-shape or occluder test (decision 37's trade). Decided by the agent, to confirm.
  - `TESWaterReflections::Update` (SE 31373 / AE 32160) runs as the engine's code, but its face render (the BSCubeMapCamera vfunc 0x35
    call, AE +0x459 / SE +0x484, 6 bytes) calls CS's recorder, which records each face (cube translate, scale, frustum, port, plain)
    and releases the cube camera's roots after the last face as the face render does. Its two renderer clear-colour calls
    (AE +0x32D / +0x493, SE +0x339 / +0x4D5) are NOPs in native: native faces clear to zero (the sky draws the background).
  - `TESWaterSystem::RenderDisplacement` (SE 31385 / AE 32176) is ported (`RenderDisplacement` thunk at the update's call): its CPU half
    (the idle timer, the restart, the wading actors' ripples into the ripple queue) and ImageSpaceEffectWaterDisplacement's Render
    (SE 0x141303da0 / AE 0x1414ef370): the clock, the texture offset step, one ripple per queued ripple (the queue arrays the game's
    AddRipple appends to are drained here), a heightmap step per 0.025 s, the displacement pair swaps and the globals the water shader
    reads (0x1433dcf0c / f10, f00, f02, f08). Each step keeps the param's float4 ids as the engine would hand them to BuildRows; a
    ripple keeps its two vertex rows. The effect's ripple quad (+0x90) is made with the engine's creation function (SE 75659 / AE 77465)
    when the first ripple needs it, as the effect does (decision 68: object creation).
- the flow-map ring (S16): drained on the game thread (the inlined pop ported); a loaded entry returns to the pool, an unloaded one is
  pushed back. When the engine's copies are still in RenderWaterEffects (Unified Water removes them; checked by the dispatch site's
  byte) each copy is recorded with its source view held by reference.

What the update asks to draw is queued with the snapshot id (`Work`: faces, displacement steps, flow copies, the cube clear). The render
side drains every item up to the adopted snapshot at CS's point and draws it once (decision 62's rule for UI uploads): a repeated snapshot
draws nothing again, and replaced snapshots' work joins the next render (faces newest per face, the newest update's displacement steps
after any restart's clear, every flow copy). The queue folds its oldest item into the next past 64 items, keeping copies and clears.
Decided by the agent, to confirm: the queue instead of a snapshot block, since this work must be applied once and must not be lost.

CS's point is DCLF's frame start (Main::Draw's call of SetupSunAndSky, +0xD3), where nothing else draws between it and the old call:
`WaterEffects::Render` draws the faces (`ReflectionFaces::DrawRecorded`), then stages the displacement steps and flow copies as start
steps with CS's rows (`ImageSpace::StageNativeWater`, `NativeStart.inl`). The restart's clear of kWATER_DISPLACEMENT (0.5) is a native
ISWaterDisplacementClearSimulation start step (registered in native only; its shader writes 0.5, and the capture checks the engine's
constant is 0.5). Water Reflection Cubemap Fix's scene check moved to the capture and its clear to the render point.

A face DCLF cannot draw natively (not plain, or its LOD roots not all DCLF's) is not drawn; a plain one still goes to the reflection
view (FaceOriented), as the engine's face render handed it over through Orient, so the view's coverage starts.

### 2. Main::SetupSunAndSky (SE 35562, AE 36561)

CPU work on the scene graph and camera (the sky nodes moved to the camera, the FOV set on the world scene graph, the culler setup that
S6 already patches, DrawWorld's player data, the focus-shadow list). The capture runs it on world snapshots before the main camera is
captured (the FOV changes the camera's frustum). DCLF's thunk at Main::Draw's call (+0xD3) no longer calls it in native: it starts
DCLF's frame, runs the water render point and returns the value the capture's call returned (Main::Draw keeps it in R15: a sky object).

### 3. Main::RenderPrecipitation (SE 100426, AE 107144) and the occlusion maps

In native none of the occlusion map code calls an engine render function or touches engine render state:

- `DrawcallLimitFix::RenderMask` replaces Precipitation::RenderMask (SE 25642 / AE 26184) for the maps DCLF draws. Native computes only
  its output, the occlusion projection at +0xED0: the fixed rows (SE 242842-242844 / AE 196452-196454) and the world camera's offset
  from the map's camera, times that camera's view-projection as SetCameraData builds it (`ShadowCameras::CameraMatrices`). Its
  SetCameraData, targets, accumulator submit, first-person end and clear made no difference in native (the accumulator is empty and
  nothing reads that state). Skylighting's sky occlusion map uses it too.
- `RenderPrecipitationMask` never runs the engine's cull (SetupMask) in native; a map DCLF did not draw this frame is drawn as a held
  one (no occluders).
- At Main::Draw's point (+0x3A1): DCLF's thunk (without Skylighting) and Skylighting's `RenderOcclusion` make no engine call in native;
  Upscaling's thunk no longer writes the dynamic-resolution lock around it.
- At the shadow maps point, native no longer saves and restores the shadow state's camera and the dynamic-resolution lock.

Still at record time, unchanged: Precipitation::UpdateProjection (SE 25643 / AE 26185), CPU math placing the map's camera, and the
Sky/precipitation reads (Skylighting's open phase 1 reads, decision 32). Open.

## Detector (engine-render calls per native world frame)

| Run | Before (s3-accum on the base) | After |
|---|---|---|
| native-vulkan+native-tour | 20.0 | 16.9 |
| native-vulkan+native-river | 21.9 | 17.3 |
| native-vulkan+native-interior | 19.5 | 16.9 |
| native-vulkan+quick | 33.1 | 25.4 |

Sites after (tour): Main::RenderWaterEffects and TESWaterSystem::Update (in RenderWaterEffects) are no longer entered (0).
TESWaterReflections::Update and RenderDisplacement (in TESWaterSystem::Update) read `[producer, hooked]` (decision 64's rule: the
reflection update records, RenderDisplacement is CS's port; neither draws). Main::SetupSunAndSky and Main::RenderPrecipitation still read
1.00 `[render, hooked]`: they are Main::Draw's calls of CS's thunks, which make no engine call in native; decision 70 keeps such sites
counted until CS drives Main::Draw itself. The helpers inside the update (AE 32269, 32170, 32175, Sky::GetSingleton, the add-roots)
count as helpers; AE 32189 now lands in CS's frustum test.

## Runs

All on stage `s7-water` (Dev-Fast), with `env.CS_SNAPSHOT_DETECT=1`.

| Config | Label | Result |
|---|---|---|
| native-vulkan+native-river | `s7-water-a-river` | passed; water dark: no face drawn (the reflection view's coverage never started, below) |
| native-vulkan+native-river | `s7-water-b-river` | passed; same, with the reason logged (roots 0x0) |
| native-vulkan+native-river | `s7-water-c-river` | passed; faces drawn (1,200 of 1,200 recorded); water reflects the sky as on the base |
| native-vulkan+native-tour | `s7-water-c-tour` | passed; 16.9 calls; Whiterun and Dragonsreach shots normal (the player dies in combat on both builds) |
| native-vulkan+cadence-drop-cells | `s7-water-c-dropcells` | passed; 590 updates, 196 render points, faces merged per face; shots normal |
| native-vulkan+rain | `s7-water-c-rain` | passed; occlusion maps drawn with CS's RenderMask; rain shots within the base's own run-to-run spread |
| native-vulkan+river, player wading | `s7-water-d-wade2` | passed; ClearSimulation, texture offset, wading ripple and heightmap steps taken natively (216 steps) |
| native-vulkan+native-interior | `s7-water-e-interior` | passed; dry interior takes the early-out; shots normal |
| guest+quick | `s7-water-e-guest` | passed; unchanged (65 calls) |
| native-vulkan+quick | `s7-water-e-nvk` | passed |
| native-d3d12-quick | `s7-water-e-d3d12` | passed |
| native-vulkan+river, `features.UnifiedWater=false` | `s7-water-f-nouw` | passed; 19 flow copies recorded on the game thread and staged as ISCopySubRegionCS start steps |
| guest+river, player wading | `s7-water-f-guest-wade` | passed; the engine's displacement as reference: the water around the wading player matches `s7-water-d-wade2` |

The wading runs use river's save with `--set steps=[...]` (`tgm`, `coc Riverwood`, `player.setpos x 19560`, `y -45350`, `z -120`,
`player.setangle x 35`, a shot 300 frames later) and `--set expect.shots=["wade"]`.

Shots: pixel diffs against the base are within its own run-to-run spread (combat and NPC positions differ between any two runs of these
saves); each shot was checked by eye for the water reflection, the displacement and the sky.

## Proposed decision-log entries

1. **Native's water effects are game-side producers** (decision 8 applied): Main::Draw does not call Main::RenderWaterEffects; the
   capture runs its game half on world snapshots (a port of its body: the early-out resets, TESWaterSystem::Update, the flow-map ring).
   TESWaterSystem::Update is game logic run as the engine's code, except its three render-side calls: TESWaterReflections::Update runs
   with its face render recorded (and its renderer clear colours patched out), RenderDisplacement and the displacement effect's Render
   are ported, and the water visibility test is CS's frustum test against the snapshot camera (no engine cull). The detector counts the
   update's two inner render sites as producers in native (decision 64's rule).
2. **What the water game half draws is a once-applied queue keyed by snapshot id**, like decision 62's UI uploads: reflection faces,
   displacement steps, flow copies and the cube clear are drawn once at CS's point, up to the adopted snapshot. A repeated snapshot draws
   none again; replaced snapshots' work merges into the next render (each face as the newest update placed it, the newest update's
   displacement steps after any restart's clear, every flow copy). The queue folds its oldest item past 64.
3. **Main::SetupSunAndSky runs in the capture** on world snapshots, before the main camera is captured (it sets the camera's FOV and moves
   the sky with the camera). Main::Draw's call of it is DCLF's frame start and the water render point in native; it calls no engine
   function and returns the capture's result. The site stays counted under decision 70.
4. **Native's occlusion maps make no engine render call:** CS computes RenderMask's one output, the occlusion projection (fixed rows and
   the world camera's offset, times the map camera's SetCameraData view-projection); the engine's SetupMask cull never runs in native (a
   map DCLF did not draw is drawn without occluders, as a held map); no shadow-state save and restore or dynamic-resolution lock write
   around it. Main::RenderPrecipitation stays counted under decision 70.
5. **The displacement restart's clear is a native ISWaterDisplacementClearSimulation start step** (its shader writes the engine's 0.5).

## Decided by the agent, to confirm

- The frustum water test: shape not hidden; in an exterior only shapes under a BSMultiBoundNode, tested by that node's bound (the
  engine's candidates); no portal, multibound-shape or occluder test. More water objects can count as visible than the engine's cull
  found (they get reflection slots), never fewer.
- The queue instead of a snapshot block (work that must be drawn exactly once and must not be lost).
- A face DCLF cannot draw natively (not plain, or its LOD roots not all DCLF's) is not drawn; it was the engine's face render before,
  whose draws native dropped anyway. A plain one still goes to the reflection view (FaceOriented) as the engine's face render handed it
  over, which is what starts the view's coverage; without it no face was ever drawn (`s7-water-a-river`).
- The face order: TESWaterReflections::Update sorts its faces by the time each face render took (QueryPerformanceCounter around the
  call). The recorder takes almost no time, so the order only follows the engine's sort of near-zero times. It decides which two faces an
  update asks for, not what they show.
- The ripple quad (the effect's +0x90, an engine BSTriShape the effect holds for the process's life) is latched at record time, as the
  water shapes and blood geometry are (decisions 90 and 105).
- A ripple step's rows: its two vertex rows at c1 and c2 (TranslationMatrix0/1), which the ORG vertex stage reads from the step's rows.
  The capture path before took only the pixel rows for a ripple.
- When several updates' displacement steps reach one render (dropped snapshots), only the newest update's are drawn, so the start steps'
  instances fit one frame. The engine stepped nothing on updates it did not render either.
- Water Reflection Cubemap Fix's scene check marks the scene seen even when the cube faces are missing (it used to retry the next frame).

## Open items

- The water port's per-frame values (`WaterPort::CaptureFrame`) still read engine globals at record time, among them those the water
  update writes (the sun and colour globals, the displacement target 0x1433dcf0c, the underwater state). They are consistent today (the
  update runs in the capture before the render half); phase 7 needs them in a block.
- The water normal textures (AE 32170) are assigned on the game thread now; DCLF's water scene reads the shader property's textures at
  record time. Single-threaded this is consistent (no update between capture and render). Phase 7 needs them versioned through the retire
  queue: DCLF part 2's design (`snap6-life.md`).
- Precipitation::UpdateProjection (SE 25643 / AE 26185) is still called at record time to place the occlusion maps' camera, and
  Skylighting and DCLF's occlusion code read Sky and the precipitation object live (Skylighting's open phase 1 reads, decision 32).
- SE is mapped (every id from Ghidra's SE disassembly) but not run.
- Main::SetupSunAndSky and Main::RenderPrecipitation still count 1.00 each (decision 70). Their native bodies make no engine call; they
  could be reclassified as CS points the way decision 64 reclassified DrawInterface, which would take the world count to about 15.
