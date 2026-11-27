# snap7-reads: the render half's live engine reads (phase 7, open items 3 and 4)

Lead `reads`, round `snap7`, branch `snap7/reads` (base `org_integration` 0ec832b27, `org_integration` merged again after snap7-ticket
and snap7-lights landed), stage `s7-reads`. Open items 3 ("the remaining points' reads") and 4 ("other features' record-time reads") of
[snap7-split.md](snap7-split.md), the open items of [snap7-lights.md](snap7-lights.md), and DCLF's points other than the frame start
([snap7-ticket.md](snap7-ticket.md) owns `FrameDriver::GameFrameStart` / `BeginFrame`). Still today's single-threaded frame: no render
thread.

Status: **done** for the reads listed in section 2; what is left, with owners, is section 5.

## 1. Measuring the render half

The live-read detector counted reads per site, thread and frame kind, not per half of the frame. It now also counts, per site, the live
reads made inside an `EngineReads::RenderHalfScope` in a world frame (outside the ImGui frame, whose reads snap7-prep2 counts):

- `Half::Record`: the native driver's render half (`Native::Scene::RecordFrame`: the shadow maps, depth, masks, precipitation, the
  clears, RenderWorld, the first-person view, SAO, the post chain, the splatter, and everything their hooks record);
- `Half::FrameEnd`: the Present hook's body (DCLF's BeforePresent, State::Reset, the ImGui overlay is excluded).

Under `CS_SNAPSHOT_DETECT=1` the log has `[Snapshot] render-half reads (record|frame end) ...: per world frame <by category>; of
game-mutated objects <by category>` and one `render-half read site (...)` line per site, tagged `(render-owned)` for render-owned objects.
The status report has `engine_reads.render_half_per_world_frame.{record,frame_end}` with a `game_mutated` sub-object.

**Render-owned objects** (decided by the agent, to confirm; snap7-split section 2): `Renderer` (the target tables, which CS's target set
writes in native), `GraphicsState` (BSGraphics::State), `ShadowState`, `Shaders` (state flags, current shaders, the utility shader),
`BoundState` (the D3D11 context's bound state, CS's own in native) and `Accumulator`. Only render code writes them in a native world
frame once the game thread runs no engine render code (phase 7 item 5, Renderer::Begin's engine body). They are counted and listed, but
the work list is the game-mutated objects (Sky, TES, Player, Camera, ShaderManagerState, ImageSpace, UI, Data, Water, Calendar, Ini,
Timer, ShadowLights, FrameGlobals).

## 2. What moved

Pattern: a game value the render half read is taken on the game side, and the render half reads the taken value. Three carriers:

- **Snapshot blocks** (the capture, both modes fill them): the sky's mode and climate (Skylighting's and Cloud Shadows' blocks), the
  ImageSpace block's blended data, the water port's game block, the core frame (`interior`, `worldLoaded`, menus, the main camera).
- **DCLF's frame-start ticket** (`FrameGlobals`, made in `FrameDriver::GameFrameStart`): new frame-globals contributions (Effect rows:
  the sun's and cloud light's colour, the weapon blood globals; Grass: wind, the sun, the camera, alpha params; Tree wind: the clock
  inputs) and new `FrameGlobals` fields (ExtrasFrame's land blend and ProjectedUVParams3; native's pipeline frame). The contribution
  space grows from 8 to 64 floats.
- **The native driver's game half** (`Native::Scene::Prepared`, after the queued lights and the shadow casters): the shadow light set
  (snap7-lights) and now Light Limit Fix's scene lights (`LightLimitFix::CaptureLights`, `LightLimitFixLights`).

| Site (baseline line) | What it read | Now |
|---|---|---|
| `SkylightingIntegration.cpp:21` | Sky mode | Skylighting block `fullSky` |
| `CloudShadowsIntegration.cpp:24` | Sky mode, climate | Cloud Shadows block `fullSky`, `climate` |
| `EffectFrame.cpp:62` (x2) | ShadowSceneNode sun / cloud light, ISM HDR; blood globals (raw) | frame-globals contribution "effect rows" |
| `Deferred.cpp:706` | the sun shadow light's splits and light transforms | light set: `SunSplits` plus the sun's descriptors after the bookkeeping (`SunSplits::descriptors`, new) |
| `WaterDraw.cpp:175/178` | TES / player cell interior test | water port game block `cubemapFixInterior` (native) |
| `PostChain.cpp:402` via `WaterDraw MaskVariant` | ISM reflections flag, frame counter | `WorldPass::NormalsTarget()` (the game half's in native) |
| `SkyNative.cpp:380/415` | ISM +0xE4 sky brightness | ImageSpace block `inputs.At(0xE4)` (native) |
| `TextureStreamingFeature.cpp:46` | world camera | snapshot main camera (native) |
| `OcclusionMapsFeature.cpp:31` | world camera height | snapshot main camera (native) |
| `RenderWorld.cpp:114` | world camera, only passed to the sun query | dropped: inside RenderWorld the query sets no camera |
| `ShadowMaskNative.cpp:248` | ISM's screen quad | read once (made with the manager, never replaced) |
| `Utils/Game.cpp:235` | ISM's TAA shader (Get/SetTemporal) | read once (same) |
| `CSEditor.cpp` preload (`EditorWindow.cpp:171/2574`) | UI menus, player cell | snapshot `worldLoaded` and menus (native) |
| `Uploads.cpp:400` (a report counter) | TES interior | snapshot `interior` |
| `GrassTables.cpp:74/102` (+ raw wind, clamp, fog, alpha globals) | sun light, world camera, globals | frame-globals contribution "grass" |
| `TreeWindScene.cpp:47` (+ raw tree manager, wind source, timers) | world camera, tree globals | frame-globals contribution "tree-wind-frame"; a repeated snapshot's render steps no clock time |
| `LightingConstants.cpp:166` + raw State timer, grid centres, tilings | posAdjust, shader state globals | `FrameGlobals::landBlend`, `projectedGlobals`; the projection at the frame camera's eye (native) |
| `GeometryPort.cpp:141` (x2) + raw ambient, SSR, world map, ambient specular | sun light, ISM HDR, globals | `FrameGlobals::pipeline` (native; eye zero inside RenderWorld, as before) |
| `LightLimitFix.cpp:749` + the whole light walk (raw) | ShadowSceneNode's active lights, their NiLights, rooms, mask channels | `Prepared::pointLights` (native); guest and engine-drawn frames read the same values now through `ReadLights` |
| `LightLimitFix.cpp` CopyLocalShadowMaps (smState, descriptors, light kind) | shadow lights | light set (`RenderSet()`); the update's seconds from the snapshot |
| `ExponentialHeightFog.cpp:583` | shadow lights' descriptors | native reads no light (see 4, EHF-a) |
| `LightLimitFix.cpp:1568/1600/1601/1609` TrackLocalShadowCasters (local shadows on) | ShadowSceneNode, player, process lists' actors, world camera, active shadow lights' NiLights | `LightLimitFixLights::actors`, `shadowLights` (native, while the cache's hooks are installed) and the snapshot's main camera |

Guest reads the same values at the same points as before: every native branch above is gated on the native driver's render half
(`Scene::Current().valid`), the light set (`RenderSet()`, null in guest), `Snapshot::FrameCamera::Owned()` or `Native::Active()`, or reads
a block guest fills at its capture. The frame-globals contributions are captured by DCLF's frame-start game half in both modes, at the
SetupSunAndSky point, before the commits that read them; their values (wind, sun, tree and grass globals, the world camera) do not change
between that point and the commits.

## 3. Cadence

- Tree wind's clock inputs come from the ticket, and a render of a repeated snapshot steps no clock time (`deltaTime` 0 on
  `Snapshot::Render().repeat`): the clocks step once per update (decided by the agent, to confirm: before, a repeated render stepped the
  clocks again by the same seconds).
- Everything else moved is a value of the update (lights, sun, camera, globals): a repeated render reads the same values; a dropped update
  is never read.
- Light Limit Fix's local shadow fade uses the snapshot's `gameDelta` in native (was the engine's per-frame seconds global, the same
  value at the same update).

## 4. Detector numbers (native-vulkan+quick, Dev-Fast, per world frame)

Render half, live reads (`render-half reads (record)`), before (s7-reads-base-nvk-quick, org_integration 0ec832b27 plus the counter)
and after (s7-reads-b-nvk-quick and s7-reads-c-nvk-quick):

| | before | after (b) | after (c) |
|---|---|---|---|
| record, game-mutated objects | 59.5 | 11.9 | 10.9 |
| record, render-owned objects | 193.2 | 190.7 | 188.9 |
| frame end, all (8.7 game-mutated) | 13.4 | 13.4 | 13.4 |
| whole run, live reads | 621 | 538 | 545 |

Per game-mutated site in the record half (quick unless named):

| Site | Object | before | after | Note |
|---|---|---|---|---|
| SkylightingIntegration.cpp:21 | Sky | 6.11 | 0 | block |
| EffectFrame.cpp:62 (two reads) | ShaderManagerState | 9.18 | 0 | ticket |
| Deferred.cpp:706 | ShaderManagerState | 3.74 | 0 | light set |
| WaterDraw.cpp:175 / :178 | TES / Player | 3.06 / 3.06 | 0 | water port block |
| TextureStreamingFeature.cpp:46 | Camera | 2.25 | 0 | snapshot camera |
| PostChain.cpp:402 | ImageSpace | 2.02 | 0.03 | the rest is ImageSpace's step-lag check (left, 5) |
| Utils/Game.cpp:235 | ImageSpace | 2.00 | 0 | read once |
| SkyNative.cpp:380 | ImageSpace | 1.99 | 0 | ImageSpace block |
| CloudShadowsIntegration.cpp:24 | Sky | 1.52 | 0 | block |
| EditorWindow.cpp:171 (x2), :2574 | UI, Player | 3.00 | 0 | snapshot |
| ShadowMaskNative.cpp:248 | ImageSpace | 1.00 | 0 | read once |
| GeometryPort.cpp:141 (x2) | ShaderManagerState | 2.00 | 0 | ticket |
| LightLimitFix.cpp:749 | ShaderManagerState | 1.00 | 0 | driver game half |
| ExponentialHeightFog.cpp:583 | ShaderManagerState | 1.00 | 0 | no light read |
| RenderWorld.cpp:114 | Camera | 1.00 | 0 | dropped |
| GrassTables.cpp:74, :102 | ShaderManagerState, Camera | 1.54 | 0 | ticket |
| TreeWindScene.cpp:47 | Camera | 0.77 | 0 | ticket |
| Uploads.cpp:400 | TES | 0.77 | 0 | snapshot |
| OcclusionMapsFeature.cpp:31 | Camera | 0.72 | 0 | snapshot camera |
| ScreenSpaceGI.cpp:826 | ImageSpace | 1.00 | 0 (c) | read once |
| LightLimitFix.cpp:1568/1600/1601/1609 (llf-local-shadows) | SMState, Player, Data, Camera | 3.40 | 0 (c) | driver game half |
| WaterNative.cpp:675 | Data | 3.00 | 3.00 | left: DCLF scene work (5) |
| WetnessEffects.cpp:984 | Sky | 2.00 | 2.00 | left: precipitation (5) |
| Skylighting.cpp:819, :873 | Sky | 2.00 | 2.00 | left: precipitation (5) |
| OccluderRules.cpp:82 | Sky | 1.00 | 1.00 | left: precipitation (5) |
| SkyNative.cpp:1021 (now :1035) | Sky | 1.00 | 1.00 | left: DCLF scene work (5) |
| FrameView.cpp:285 (now :291) | UI | 1.00 | 1.00 | left: DCLF load state (5) |
| Skin Surfaces.cpp:122 | Player | 0.77 | 0.82 | left: DCLF material work (5); interior 6.4 |
| GrassTables.cpp:402 (now :445) | Ini | 0.01 | 0.01 | a cached setting pointer, read once |

The detector undercounts (snap0-detect, "Not covered"): raw `REL` globals and walks through engine pointers are not sites. The moves above
also took those reads with them where the code had them (Light Limit Fix's whole light walk, the Effect, Grass and Tree wind globals,
ExtrasFrame's State globals, the pipeline frame's globals).

## 5. What is left, and who owns it

**Engine reads of DCLF's scene work during the render half, under the EngineReadWindow hold (decision 140).** Phase 7 removes the hold,
so each needs its values taken on the game side first (owner: a DCLF lead, the ticket's next round):

- Water scene passes (`WaterNative.cpp ScenePasses`, :675): the tracked water shapes' geometry and properties, `Reached(worldRoot)` and
  the pass derivation from the live property, at each water batch. Needs the pass list (or the shapes' captured property values) in the
  ticket.
- Sky root passes (`SkyNative.cpp RootPasses`, :1035): the face cull walks the sky root and its cloud layers at record time. Needs the
  sky's draw list as values (the Cloud Shadows block already holds the layers as keys).
- Skin material resolution (`Skin::Observe` via `ResolveGeometryMaterial`, Surfaces.cpp:122): geometry parents, user data, the actor's 3D
  and the property's material and textures, when a material port asks. Needs a game-side per-geometry key cache (the ticket's shading
  items carry most of it).
- DCLF's load state (`FrameView::LoadShown` / `IsLoadingScreenUp`, :291, from LatePoseOverrides, LoadHold and EndFrame's
  `NoteLoadingScreen`): the live loading menu guards scene walks against a load tearing the world down. The ticket's `loading` is the
  update's; in phase 7 the guard itself must become "the snapshot is a world snapshot" plus the retire queue's keep-alive. Left live:
  changing it changes when DCLF stops walking at a load.
- DCLF's other points (snap7-split section 6, not counted by the detector): DepthCommitted (`LatePoses::ConfirmFrame` walks roots),
  EarlyPrepass (`ServeMaterialRequests` reads `property->material`), Prepass (`LatchAccumulator`), BeforePresent
  (`LatePoses::CheckAtPresent`), EndFrame (`HoldForRenderHalf`, the list graveyard). BeforeOpaquePass's `ApplyPendingState` writes
  render-owned renderer state.

**Precipitation occlusion** (owner: a precipitation lead; Skylighting.cpp:819/873, OccluderRules.cpp:82, WetnessEffects.cpp:984, and
`DrawcallLimitFix::RenderPrecipitationMask` / `RenderMask` in native): the render half finds the precipitation object through the sky,
calls the engine's `Precipitation::ComputeProjection` (AE 26185) on it, writes the precipitation shader's cube size and direction globals
and the object's `lastCubeSize`, and writes the occlusion projection into the rain emitter, which Wetness then reads back. Plan: port
ComputeProjection to a CS function of values; take the precipitation object's key, its occlusion camera's values and the rain emitter key
in the game half (Prepared), compute both maps' projections there from Skylighting's per-update direction (BeginOcclusionFrame's sample
moves with it), and hand DCLF's occlusion views and Wetness the projections as values.

**ImageSpace** (owner: the ImageSpace port leads): the world steps' lag check reads the scope fields live (`WorldFieldsNow`, PostChain.cpp:402,
0.03 a frame); Begin's `PostChain::Read` / `ReadWorld` and the post chain's description read ImageSpaceManager fields at Begin (outside
both halves today, render side in phase 7); `State::Reset` clears the ApplyReflections effect's `active` (State.cpp:432, frame end), which
the engine's own water code may set on the game side.

**Frame end** (Present hook, 8.7 game-mutated reads a frame; owner: the phase 7 Present-as-publish item):
- Engine fixes' per-frame updates: `LODBlendRingFix::Update` (TES, player, the shader state's grid centres; writes the grid centres and
  TES's blend flag) and `SunShadowMaskFix::Update` (the shadow node's sun and its slots; the result feeds the engine's GetRenderPasses).
  Both write or decide game-side state and belong in the game-side publish (decision 34's side effects), not State::Reset. Moving
  LODBlendRingFix shifts its effect by one update (it would see the update's grid shift before the frame instead of after).
- `SubsurfaceScattering::Reset` writes the shader state's character light flag and strength from settings every frame: a settings write
  for the game thread (`GameThread::Run`).
- DCLF's EndFrame load note and LoadHold (UI, above), the test harness's player read (`Testing/Lifecycle.cpp:133`, the harness split,
  snap7-split item 9).

**Render-owned objects** (190 a frame in quick: the Renderer's target tables, BSGraphics::State, the shadow state, the shader state flags,
the bound state): left by design (section 1). They stop being shared once Renderer::Begin's engine body and DrawFrame's engine helpers
leave the game thread (phase 7 item 5). Two of them carry game values and should move with that item: `Util::GetEyePosition`
(`Utils/Game.cpp:191`, posAdjust) and Light Limit Fix's strict lights (`SetLightPosition(..., false)`), which mean the main camera's eye
(the frame camera has it); the depth pass's Terrain Blending posAdjust reads likewise.

**Not in the render half today but render side in phase 7:** the frame driver's Begin (`SampleEngine`, item 7) and ImageSpace's Begin
reads; the ImGui frame's reads (snap7-prep2's list).

## 6. Runs

Stage `s7-reads`, Dev-Fast, `CS_SNAPSHOT_DETECT=1` on native runs.

| Config | Label | Build | Result |
|---|---|---|---|
| native-vulkan+quick | s7-reads-base-nvk-quick | base + counter | passed; render half record 59.5 game-mutated, 193.2 render-owned |
| native-vulkan+quick | s7-reads-a-nvk-quick | a (blocks, ticket rows) | passed with the shot reference; 16.0 game-mutated |
| native-vulkan+quick | s7-reads-b-nvk-quick | b (+ lights, pipeline, extras) | passed with the shot reference; 11.9 |
| guest+quick | s7-reads-b-guest-quick | b | passed with the shot reference |
| native-vulkan+native-interior | s7-reads-b-native-vulkan-native-interior | b | passed with shot references |
| native-vulkan+llf-local-shadows | s7-reads-b-native-vulkan-llf-local-shadows | b | passed (6 shots) |
| native-vulkan+native-river | s7-reads-b-native-vulkan-native-river | b | passed |
| native-vulkan+cadence-drop-cells | s7-reads-b-native-vulkan-cadence-drop-cells | b | passed (3 shots) |
| native-vulkan+shadows-blend | s7-reads-b-native-vulkan-shadows-blend | b | passed (7 shots) |
| native-vulkan+llf-local-shadows-fog | s7-reads-b-native-vulkan-llf-local-shadows-fog | b | passed |
| native-vulkan+native-tour | s7-reads-b-native-vulkan-native-tour | b | passed |
| native-vulkan+rain | s7-reads-b-native-vulkan-rain | b | passed |
| native-vulkan+first-person-switch | s7-reads-b-native-vulkan-first-person-switch | b | passed (7 shots) |

## 7. Decided by the agent, to confirm

- RD-a. **Render-owned objects are not the work list** (section 1): the detector splits the render half's reads into game-mutated and
  render-owned objects; "done" here means no game-mutated object is read while recording, apart from the listed DCLF scene work under
  the hold and the precipitation port. Why: snap7-split section 2 already classes that memory as the render thread's once item 5 lands, and
  moving the target tables into a capture would copy CS's own writes back to itself.
- RD-b. **Three carriers, picked by when the value is final**: snapshot blocks for values final at the capture (sky mode, ImageSpace data,
  the camera), DCLF's frame-start ticket for DCLF's own frame values (taken where DCLF's frame start already reads the engine), the native
  driver's `Prepared` for values that change inside the game half (lights: the queued lights are processed and the casters select after
  the capture). Light Limit Fix's lights are taken right after the light set for that reason.
- RD-c. **Never-replaced engine objects are read once** (the ImageSpace manager's screen quad, TAA shader, SAO blur shader): their pointers
  are not game state; the fields CS writes on them are CS's alone.
- RD-d. **Exponential Height Fog reads no shadow light in native and writes no engine local shadow**: that is what it did before (every
  light's maps were given back before its Prepass, in guest by the engine's masks, in native since snap7-lights by the capture), so the
  images do not change. The light set's pre-return values would give EHF real engine local shadows: a visual change to decide separately.
- RD-e. **Tree wind's clocks step once per update** (a repeated snapshot's render passes 0 seconds).
- RD-f. **ExtrasFrame's ProjectedUV projection is placed at the frame camera's eye in native** (it read the shadow state's posAdjust at each
  commit): the same value at every main-camera commit; a reflection face's commit now uses the main eye instead of the face's, where the
  projected-UV offset of a cube face moves by the face camera's offset from the player camera (the river, tour and rain runs passed).
- RD-g. **DCLF's load-state reads stay live** (FrameView::LoadShown, NoteLoadingScreen): they guard scene walks against loads; moving them is
  part of removing the hold.
- RD-h. **Frame-globals space grows from 8 to 64 floats** (51 used: first-person eye 4, tree wind 4, Effect rows 12, Grass 21, Tree wind
  frame 10).

## 8. Proposed decision-log entries

- **The detector splits the render half** (`EngineReads::RenderHalfScope`, `Half::Record` around `RecordFrame`, `Half::FrameEnd` around the
  Present hook): per-site counts and `render_half_per_world_frame` with a game-mutated sub-total; Renderer, GraphicsState, ShadowState,
  Shaders, BoundState and Accumulator are render-owned (RD-a).
- **Native's render half reads game values from three carriers** (RD-b): blocks (sky mode for the Skylighting and Cloud Shadows bindings,
  ImageSpace data for the sky brightness, the water port block for the cubemap test, the core frame for interior, world and menus, the
  snapshot camera for texture streaming and the occlusion floor), DCLF's ticket (frame-globals contributions for the Effect, Grass and
  Tree wind rows; `FrameGlobals::landBlend`, `projectedGlobals`, `pipeline`), and `Prepared` (Light Limit Fix's scene lights and the
  local shadow cache's caster inputs, `LightLimitFixLights`). Deferred's sun cascades and Light Limit Fix's local shadow copies read the
  light set. Game-mutated record-half reads in quick: 59.5 -> 11.9 per world frame (render-owned 193 -> 191).
- **Left after snap7-reads:** DCLF's scene work under the hold (water scene passes, sky root passes, skin material resolution, the load
  state), the precipitation occlusion port, ImageSpace's step-lag check and Begin reads, the frame end's engine-fix updates and settings
  writes (section 5).
| native-vulkan+quick | s7-reads-c-native-vulkan-quick | c (+ caster tracking inputs, SSGI's shader read once) | passed with the shot reference; 10.9 game-mutated |
| native-vulkan+llf-local-shadows | s7-reads-c-native-vulkan-llf-local-shadows | c | passed (6 shots); caster tracking reads 3.4 -> 0 |
| guest+quick | s7-reads-c-guest-quick | c | passed with the shot reference |
| native-vulkan+screen-blood | s7-reads-c-native-vulkan-screen-blood | c | passed (blood rows from the ticket) |
| native-vulkan+upscalers | s7-reads-c-native-vulkan-upscalers | c | passed (DLSS, FSR, TAA, XeSS) |
| native-vulkan+post-effects | s7-reads-c-native-vulkan-post-effects | c | passed (13 shots) |
| native-vulkan+menu-scenes | s7-reads-c-native-vulkan-menu-scenes | c | passed with shot references |
| native-vulkan+menu-loading | s7-reads-c-native-vulkan-menu-loading | c | passed |
| native-vulkan+update-cap30 | s7-reads-c-native-vulkan-update-cap30 | c | passed |
| native-vulkan+update-cap240 | s7-reads-c-native-vulkan-update-cap240 | c | passed |
| native-vulkan+window-resize-stress-taa | s7-reads-c-native-vulkan-window-resize-stress-taa | c | passed (13 shots) |
| native-vulkan+native-interior | s7-reads-c-native-vulkan-native-interior | c | passed with shot references |

Skipped (director's 2026-11-27 test scope): native-d3d12-quick (no ORG, BasicRHI or native device code changed). Build c changed only
Light Limit Fix's caster tracking, SSGI's pointer read and two formatting lines after build b, so native-tour, native-river, rain,
shadows-blend, llf-local-shadows-fog, cadence-drop-cells and first-person-switch stand on build b.
