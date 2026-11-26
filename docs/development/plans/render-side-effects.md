# Render-half side effects on the game: inventory

Status: phase 0 item 5 of [render-thread-snapshots.md](render-thread-snapshots.md), 2026-11-26. Research only, no code
changed. Method, scope and open questions: [snap0-effects.md](snap0-effects.md).

Native will skip `Main::DrawFrame` (AE 36555 / SE 35556) and everything it calls. This file lists every write that the
render half makes and something outside it reads. For each write it says who reads it, what breaks if the write stops,
what CS does today, and a suggested fate. **Every fate is a suggestion.** The user decides each one per feature.

Conventions:

- Ids are AE 1.6.1170 / SE 1.5.97 address library ids. "SE ?" means the SE id was not resolved. Addresses are AE.
- Names in quotes are our own names for unnamed functions.
- **Confidence:** *verified* means the write and at least one reader were seen in a decompile. *Inferred* says what it
  rests on.
- **Thread:** DrawFrame runs on the main thread, called from Main::Update (AE 36564) at +0xA97. "Jobs" means the job
  workers DrawFrame starts.
- **CS today** names where CS writes the value in native today, and whether that write sits on the render path (a phase 2
  problem) or on the game thread.

## Summary

| # | Write | Game readers | Effect if it stops | CS today | Suggested fate | Confidence |
|---|---|---|---|---|---|---|
| S1 | Job stage events 5-9 (`SetJobTaskEvent`) | Stage jobs: shadow-cull wait, JobListEnd, "Non render-safe AI" (player 3D update) | Stage jobs block; Main::Update likely hangs next frame | nothing | CS signals them on the game thread | verified (hang inferred) |
| S2 | `gFrameCounter` / `State::frameCount` ++ (Renderer::End) | animation rate test, decal previous transform | animation rate test breaks | read only | CS increments once per update on the game thread | verified |
| S3 | `gIsInMenuMode2` | Papyrus update timers, AIProcess 3D update routing, reference timers | Papyrus timers run during freeze-background menus | read only | CS writes it on the game thread | verified |
| S4 | Render-unsafe thread mask, cleared after the world draw | `TaskQueueInterface::ShouldUseTaskQueue` (AI 3D updates on jobs) | job threads always defer 3D work | nothing | needs the user's call | verified (effect inferred) |
| S5 | Fade enable globals 20 frames after a teleport | BSFadeNode, Papyrus Enable/Disable fade, tree manager | fades never come back on after the first load or teleport | read only | CS counts frames on the game thread | verified |
| S6 | Actor pre-cull: actor 3D `kNotVisible` (bit 20), `Actor+0x27C` bits | animation rate, ragdoll IK, detection gate, VATS, head tracking, special idles, facegen visible | stale visibility for AI and animation | **nothing** | CS produces it on the game thread | verified |
| S7 | `kAccumulated` (bit 26) on scene nodes | animated-reference update, tree wind clock | controllers and managed nodes stop animating | `Visibility.cpp` at Main::Update, from GPU readback; DCLF roots only | CS produces it for every root, game thread | verified |
| S8 | BSFadeNode fade and LOD fields | animated-reference visibility test, pre-cull, tree LOD | fades freeze or snap | `FadeWriteBack.cpp` at Main::Update; DCLF roots only | CS fade step for every root | verified |
| S9 | `BSShaderProperty::lastAccumulatedFrameCount` (+0x80) | animation rate of off-screen actors | shadow-casting actors animate slowly | `ActorRate.cpp` at Main::Update (shadow views only) | CS stamps, game thread | verified |
| S10 | Portal visibility map | `Main::IsRoomVisible` (managed nodes, ref lights, detection gate) | portal-shared nodes count as not visible | detoured, answered from game data | keep | verified |
| S11 | Multibound cull result | tree manager LOD loop | trees skipped | `TreeAnimation.cpp` around the loop | keep | verified |
| S12 | ShadowSceneNode `activeLights` / `activeShadowLights` (queued add/remove) | **stealth light level** (player), detection | new lights never count; removed lights keep counting; queues grow | nothing on the game thread | CS runs or rebuilds it on the game thread (user's call) | verified |
| S13 | Per-geometry light lists (`lightData->lights`) | **stealth light level** (NPCs) | NPC light level keeps old lights | LLF gate on the render path | as S12 | verified (chain); one callee inferred |
| S14 | World camera globals `worldToCamMatrix` / `viewPort` (AE 406126 / 406160) | HUD floating quest markers; SKSE HUD mods | markers freeze or go wrong | nothing | CS writes them unjittered on the game thread | verified (mods inferred) |
| S15 | Save screenshot handshake (ImageSpaceManager +0x211 / +0x218) | save request pump | some saves (request bit 29) are re-queued forever and never written | read only | CS produces it | verified (bit 29 meaning inferred) |
| S16 | BGSAutoWater flow-map ring drained, pool entries freed | cell loader's flow-map producer (spins on `Sleep`) | **cell loading hangs** after about 128 flow-map cells | nothing | CS drains the ring on the game thread | verified (pool size inferred) |
| S17 | Decal occlusion queue (state 3 to 4, `+0x60` cleared) | BGSDecalManager update (Main::Update) | decals stuck; queue blocks; references held | detour fires only when DrawFrame calls it | needs the user's call | verified |
| S18 | Facegen tint bake queue | none wait on it | face tints never fill; queue leaks | nothing | CS drains and bakes | verified |
| S19 | Sun occlusion result (sun accumulator +0x68) | `Sun::Update` glare scale | glare freezes | `SkyHooks.cpp` answers, but the poll runs in RenderWorld | CS writes +0x68 from its readback, or drops glare | verified |
| S20 | Underwater resets (`TESWaterSystem+0xB8`, underwater sound) | player water state, audio | underwater audio may stick in waterless interiors | nothing | CS repeats the resets on the game thread | verified (staleness inferred) |
| S21 | Water ripples and water-object garbage collection | none (renderer, memory) | arrays and lists grow | nothing | CS drains on the game or render side | partly inferred |
| S22 | Post-render job stage (Sky::Update, Face morphing, shared particles, grass) | weather, sky, sun, precipitation | may stop if the stage events stop | nothing | tied to S1 | job table verified, link inferred |
| S23 | UI driven from DrawInterface: `PlayerCamera::Update` in TweenMenu, Inventory3D item load/zoom, BookMenu page movie calls | camera, menus | camera freezes in TweenMenu; item never appears; book pages blank | menu draws latched | CS runs the non-draw parts (UI port) | verified (effects inferred) |
| S24 | PrintScreen screenshot and the capture toggle (fixed timer delta) | game timer | screenshot and capture keys do nothing | nothing | needs the user's call | verified |
| S25 | `BSGraphics::State` camera data, jitter, shadow bookkeeping, first-person shift, focus-shadow list | renderer only | none for the game | CS writes several on the render path | goes away | verified / inferred per row |

The most serious are S1 (possible hang every frame), S16 (cell loader hang), S15 (saves never written), S12/S13
(stealth), S6 (AI and animation visibility, which CS does not cover at all) and S5 (fades off after a teleport).

## 1. Frame sync, counters and main flags

| Write | Writer (AE / SE, when, thread) | Readers outside the render half | Effect if stopped | Renderer-only? | CS today | Suggested fate | Confidence |
|---|---|---|---|---|---|---|---|
| S1. Stage events, group 1: 5, 6, 7, 8, 9 | `SetJobTaskEvent` AE 69514 / SE 68165. Event 5 in CalculateAndDrawShadowCasterLights (107132 +0x144). Event 6 in RenderPlayerView after RenderWorld. Event 7 after the first-person view (or at once with none). DrawFrame sets 6 and 7 itself when no world is drawn, 8 after DrawInterface, 9 after Renderer::Unlock. Main thread | "Wait For Shadow Culling" (0x1406d2800, event 5); "JobListEnd" AE 39076 (6, or 8 in freeze menus); "JobListEnd" 0x1406d3640 (7); "Non render-safe AI" via AE 40522, which updates the player's third-person 3D, waits on 7, then the first-person 3D; "JobListEnd" AE 39094 (8 then 9). Main::Update waits for the pipeline end | Stage jobs block; the next Main::Update probably hangs | No: a sync contract | nothing | CS signals the same events on the game thread at equivalent points | verified (sets and waits from disassembly); the hang and reset semantics inferred |
| S2. `gFrameCounter` AE 411489 / SE 525008 (0x14328cc6c, `State+0x4C`) ++; `insideFrame` (+0x54) | Renderer::End AE 77246 / SE 75461 (+0xBB). Also from the loading loop and the Bink tick. Begin AE 77245 sets `insideFrame` | `Actor::ModifyAnimationUpdateData` AE 38054 (`frameCount - stamp < threshold` picks the animation rate); `BGSDecalNode::UpdateWorldData` AE 70539 (once-per-frame previous transform). About 20 renderer functions | Stamps compare against a counter that no longer moves: animation rate and decal previous transforms go wrong | No | read only: `ActorRate.cpp:29,57,100`, `MasterParticles.cpp:52`, `EngineDrawFamily.cpp:221` | CS increments it once per game update on the game thread (render code then reads the snapshot's copy) | verified |
| S3. `gIsInMenuMode2` AE 403436 / SE 516934 (0x14318714a) | DrawFrame: 0 when no menu freezes the background; 1 after the frozen-world capture (RenderScene mode 0xE, helper AE 36557). AE 36608 clears it on load | `SkyrimVM::ProcessRegisteredUpdates` (0x1409bfb00, menu-excluded timer +0x690); `AIProcess::Update3DModel_Impl` (0x1406e3b70, task queue in menu mode); AE 40747 (reference timer +0x8DC); one unnamed reader at 0x140a4dcb9 | Papyrus RegisterForUpdate timing and reference timers keep running in freeze-background menus; 3D updates not deferred | No | read only: `Core/Render/FrameDriver.cpp:82` | CS writes it on the game thread from Main::FreezeTest (82075) | verified |
| S4. Render-unsafe thread mask AE 404075 (0x14319aec0) | Main::Update sets the main thread's bit (AE 39034 with 1). RenderPlayerView clears it after the world draw (39034 with 0; SE 38080) | `TaskQueueInterface::ShouldUseTaskQueue` (0x1406d1f00), used by `AIProcess::Update3DModel` and stage jobs | Job threads always defer 3D work to the task queue, as in frozen menus | No | nothing | Needs the user's call: where CS clears it | write and reader verified; effect inferred |
| S5. Fade enable globals 0x142032dfc (AE 390865 / SE 513141), 0x142032dfd (390866 / 513142), counter 0x14332a284 (AE 414558) | AE 107143 / SE 100425 at the end of RenderPlayerView: after 20 frames with the flag clear, sets both to 1. AE 40438 (teleport/load path) zeroes it; LocalMapRenderer AE 16335 saves and restores it | BSFadeNode::OnVisible and ctor, BSLeafAnimNode::OnVisible, `SkyrimScript::EnableFunctor` / `DisableFunctor` (Papyrus fade), BSAnimationGraphManager, tree manager (AE 27094) | After the first teleport or load, fades never turn back on: objects pop, Papyrus Enable(fade) does not fade | No | read only: `Records.h:438-439` (FadeFrame) | CS counts frames on the game thread | verified (writer, the zeroing writer and readers seen) |

## 2. Visibility, culling and actor update rates

| Write | Writer (AE / SE, when, thread) | Readers outside the render half | Effect if stopped | Renderer-only? | CS today | Suggested fate | Confidence |
|---|---|---|---|---|---|---|---|
| S6. Actor 3D root `flags` bit 20 `kNotVisible` (also set when out of view or occluded); `Actor+0x27C` low bits: 1 occluder-tested, 2 visible, 4 root `currentFade` above threshold | `HighActorCuller::Operate` AE 40335 (0x140729a90) / SE ?, driven by AE 40336 / ProcessLists AE 41326, from AE 36561 / SE 35562 ("Main::SetupSunAndSky") called by RenderPlayerView +0xD3. Gated by `bPreCullActors:General`. Uses the world camera frustum, the occluder test AE 106433 and the fade-node culler. Main thread | Bit 20: `Actor::ModifyAnimationUpdateData` AE 38054; AE 41381 / SE 40367 (actor update under `ProcessActorAnimSG`: detection-event expiry, per-process update only when visible); `Actor::UpdateFadeSettings` AE 37992 / SE 36201; ragdoll IK AE 41375 and `UpdateRagdollPostPhysics` AE 41447; VATS AE 41246. `+0x27C`: `Actor::ProcessTracking` AE 38009 (head tracking), `AIProcess::RandomlyPlaySpecialIdles` AE 39281, `IsFaceGenVisible` AE 27000, `BGSSceneActionDialogue::Unk_13`, AE 37387 and three unnamed | Stale visibility. Bit stuck clear: everything animates at full rate (cost only). New actors read `+0x27C` as 0: never "fully visible", so no special idles, head tracking flag cleared, facegen treated as hidden | No | **nothing** (`ActorRate.cpp:2` only mentions it; `PortalViews.cpp:542` reproduces the occluder test) | CS produces it on the game thread. Simplest: call the culler (AE 40336 path) at the old DrawFrame point; it needs only the world camera and occluder state that Main::Update's portal walk resets. Frame N's result is read by frame N+1's actor updates, so the order is kept | verified (writer and 8 readers decompiled) |
| S7. `NiAVObject::flags` bit 26 `kAccumulated` | `BSCullingProcess::Process1` AE 76553 / SE 74804 and the other culling processes, through `BSCullingProcess::Process` AE 106921 / SE 100213; scene-list job `DrawWorld_BuildSceneLists` AE 36643 / SE 35630; AfterFullFrustum AE 106323 / SE 99689 and the shadow list accumulation. Main thread and jobs | AE 19384 / SE 18972 (visibility test for animated references, used by `UpdateAnimationJob` and the managed-nodes update: bit 26, then `currentFade > 0`); tree wind clock AE 27095 / SE 26517 | Controllers and managed nodes on unflagged roots stop animating unless fading; tree wind stops | No | `Readback/Visibility.cpp:897-1016` `WriteAccumulated` (store :651, chain :669, cell roots :697), called from `PrimaryCull.cpp:880-891` at `Main_Update_Entry` (`DrawcallLimitFix.cpp:313-320`, hook :616-620). Game thread, but the data is the GPU readback armed on the render path (:824-865). Covers DCLF's stood-in and listed roots only (:886) | CS produces it for every root on the game thread once no engine cull runs | verified |
| S9. `BSShaderProperty::lastAccumulatedFrameCount` (+0x80) = frame counter | Pass registration for depth and shadow modes (0x1414b2a60, 0x1414b2810, 0x1414b2790), via AE 76558 and the accumulators. Main thread and jobs | `Actor::ModifyAnimationUpdateData` AE 38054: a `kNotVisible` actor animates at full rate only if its skin property (MiddleHigh +0x178) carries this frame's counter | Off-screen actors that cast shadows drop to low animation rate | No | `ActorRate.cpp:52-110` stamps actors inside last frame's sun cascades or local shadow views, at Main::Update (:122). Inputs are DCLF's render-path boards. It does not stamp actors visible in the main view | CS stamps on the game thread; meaningful only together with S6 | verified |
| S10. Portal graph visibility map (list process 0 +0x30190) | `DrawWorld_BuildSceneLists` AE 36643; `BSPortalGraphEntry::ClearVisibility` AE 76119 / SE 74395 | `Main::IsRoomVisible` AE 36618 / SE 35608 (managed nodes, `Actor::UpdateRefLight`, AE 41381 detection gate) | Portal-shared nodes report not visible | No | All three calls detoured (`Portals/RoomVisibility.cpp:27-53`), answered from `PortalViews::Build` after Main::Update's walk (`PortalsFeature.cpp:26-35`, hook at 36564+0x9BF). Game thread, game data | Keep | verified |
| S11. `BSMultiBoundShape+0x10` cull result, `BSMultiBoundNode::lastAccumTime` | `BSMultiBoundNode::OnVisible` during the culls | Tree manager loop AE 27094 / SE 26498 (Main::Update): visits a tree only when the result is 1 | Tree distance/LOD loop skips every tree | No | `TreeWind/TreeAnimation.cpp:274-300,313,334` writes it from DCLF verdicts around the loop. Game thread | Keep | verified |

## 3. Fades and LOD

| Write | Writer | Readers outside the render half | Effect if stopped | Renderer-only? | CS today | Suggested fate | Confidence |
|---|---|---|---|---|---|---|---|
| S8. BSFadeNode `currentFade` +0x130, flag bits 14/15/27, LOD blend +0x14C, levels +0x152/+0x153, last-visible frame +0x13C, +0x140, LOD metric +0x144/+0x148 | `BSFadeNode::OnVisible` AE 105485 / SE 98848, `BSLeafAnimNode::OnVisible`, `BSTreeNode::OnVisible`, through the fade update AE 105486 / SE 98849 and the leaf LOD 0x14147a430, during the culls. Main thread and jobs | AE 19384 (reads +0x130 and itself runs the fade step for unsettled, unflagged roots); `HighActorCuller::Operate` (actor root +0x130); BSTreeNode LOD | Fades freeze. Animated references then fade from a stale +0x13C and snap | Mostly (render passes, lighting fade) | `Fades/FadeWriteBack.cpp:45-100` writes flags, +0x130, and +0x14C/+0x152/+0x153 for leaf and tree roots, from `FadeWriters.cpp:239-247` at Main::Update. Events harvested from the GPU on the render path, judged on an executor task (:121-196). Hook on AE 19384's fade call (`FadeWriters.cpp:40`). Not written: +0x13C/+0x140/+0x144 | CS fade step for every root (actors and roots outside DCLF's tables too), once per snapshot | verified (CS side and AE 19384); engine side from `skyrim-engine-notes.md` "Fade state" |
| First-person root `kHidden` cleared, culled, set again | `Main::FirstPersonCulling` AE 36563 / SE ? | none (ends hidden) | none | Yes | — | Goes away | verified |

## 4. Lights and stealth light level

The stealth light level is computed on the game thread, but from light lists only the render half maintains.
`HighProcessData::lightLevel` (+0x3A8) is written by AE 39946 / SE 38900 (`AIProcess::CalculateLightValue`, named in
`InteriorSun.cpp:71`) through AE 39510 / SE 38500, from `PlayerCharacter::Update` and AE 41381. It is read by
`Actor::CalculateDetection` AE 37774, `Actor::GetLightLevel` AE 37775 / SE 36759 (Papyrus `GetLightLevel` native AE 21783
and the condition function), the `GetDetected` native and AE 37567. The player branch sums `BSLight::GetLuminance`
(AE 108292) over `ShadowSceneNode[0]` active and shadow lights (AE 106362), plus sun terms scaled by
`ImageSpaceManager+0xE0` (`sunlightScale`). The NPC branch (AE 107776) sums over the actor's per-geometry light list.

| Write | Writer | Readers outside the render half | Effect if stopped | Renderer-only? | CS today | Suggested fate | Confidence |
|---|---|---|---|---|---|---|---|
| S12. SSN `activeLights` (+0x130) / `activeShadowLights` (+0x148): applies `lightQueueAdd` (+0x160) and `lightQueueRemove` (+0x178), drops orphans, sets `allowLightRemoveQueues` (+0x300) | `ShadowSceneNode::ProcessQueuedLights` AE 106335 / SE 99701 via `ProcessAllQueuedLights` AE 106337 / SE 99703 (all four nodes), from RenderPlayerView +0x194 after the scene-list cull job. Other callers: load, cell, portal graph and UI3D paths only. Main thread | AE 39946 player branch (AE 106362, 106361): stealth and detection | `AddLight` only queues (AE 106327): new torches, spells and cell lights never count toward the player's light level; removed lights keep counting; both queues grow without bound | No | Nothing runs it on the game thread. `LightLimitFix/LightAttach.cpp:114-125` hooks the attach walk at 106335+0x6E2 (render path) | CS runs AE 106337 on the game thread, or rebuilds the lists from game data. Needs the user's call. The cull-process argument (feeds 0x141508eb0) needs checking first | verified |
| S13. Per-geometry light lists `BSShaderProperty::lightData->lights` | Light-attach walk AE 106342 / SE 99708 inside AE 106335 for added or moved lights (106401 / 106400, 108281, 108283, add via AE 107777) | AE 39946 NPC branch (AE 107776): NPC detection, `GetLightLevel` on NPCs | NPC light levels keep their last light assignment | No (renderer reads them too) | LLF attach gate skips unchanged lights for up to 60 frames (`LightAttach.cpp:22,114-125`), render path | As S12 | verified chain; AE 107777's meaning inferred |

## 5. Camera, projection and shadows

| Write | Writer | Readers outside the render half | Effect if stopped | Renderer-only? | CS today | Suggested fate | Confidence |
|---|---|---|---|---|---|---|---|
| S14. Globals `worldToCamMatrix` AE 406126 / SE 519579, `viewPort` AE 406160 / SE 519618, ready flag AE 406117 | AE 51639 / SE 50744 in DrawFrame just before RenderScene, world frames only, under its own spinlock. Copies the world camera's worldToCam (+0x110..0x14F) and port (+0x174) | `FloatingQuestMarker` (0x1409232xx, HUD quest markers). SKSE HUD mods (TrueHUD and similar) read them too | HUD quest markers freeze or misplace | No | nothing | CS writes them on the game thread after the camera update, unjittered | verified (mods: inferred from common use) |
| World NiCamera viewport (+0x174..0x180) | AE 36561 copies the global port 0x142033020, or {0,1,1,0} when `Main+0x1D8` is set | `WorldPtToScreenPt3` users | none normally: Main::Update writes the same port itself | Mostly | — | Goes away, unless the `Main+0x1D8` mode matters | verified (writes) |
| S25a. `BSGraphics::State` camera data, `cameraDataCacheA`, current camera globals, `projectionPosScale` (jitter) | `State::SetCameraData` AE 77503 / SE 75694 (RenderPlayerView, RenderShadowMasks, RenderPostResolve, Start/SubmitAccumulating AE 106437/106436 / SE 99790/99789, precipitation, UI3D); jitter at Renderer::Begin | Only shaders, ImageSpace and renderer code in the xrefs. Game projection and picking use NiCamera, which these do not write | none for the game | Yes | Render path: `PostChain.cpp:256-267`, `Upscaling.cpp:1429-1450, 1584-1597`, `SkyHooks.cpp:508`, `TerrainBlending.cpp:383`, `ShadowMapsNative.cpp:183-187, 252-258` | Goes away | inferred (xref sampling) |
| S25b. Shadow bookkeeping: free mask (AE 391715), `shadowLightsAccum` (106367 / 106365), light index +0x520, scissor +0x544-0x550, +0x558, SSN `firstPersonShadowMask` +0x248, descriptors, `sceneAccumArray` | AE 107132 / SE 100414, 107137 / SE 100419, 107133 (lights' Render), 108492, 107603 / SE 100819 | Only BSShadowLight functions and the three caster functions | none | Yes | `ShadowMapsNative.cpp:195-251` (render path). `ShadowViews.cpp:411-469` only reads | Goes away | verified (globals), inferred (SSN fields) |
| S25c. First-person shift: FP camera and node, player lights (PC+0x9A0, PC+0x1A8 x4), SSN `cameraPos` (+0x2F4), `lightingOffset` (+0x2E8) | AE 107123, AE 40573 / SE 39494 (twice), restored by AE 107124 | none found; net zero after the restore; jobs that care wait on event 7 | none | Yes | `Deferred.h:212` hooks RenderFirstPersonView | Goes away | inferred |
| S25d. Focus-shadow actor list 0x143187520 to 0x14332a488 | AE 36561 via AE 36589 (0x14064d3b0) | `BSShadowDirectionalLight`, 0x1414ccb30, `BSUtilityShader::SetupTechnique` (renderer) | — | Yes | — | Goes away | verified |

## 6. Water

| Write | Writer | Readers outside the render half | Effect if stopped | Renderer-only? | CS today | Suggested fate | Confidence |
|---|---|---|---|---|---|---|---|
| S16. BGSAutoWater (0x1420f8970) flow-map pending ring (+0x1008, 256 slots) drained; entries returned to the free pool (+0x1818) | `Main::RenderWaterEffects` AE 36560 / SE 35561 copies each cell's `Flow.x.y.dds` into the `Flowmap` texture (0x14332a460) and frees the entry. Its early-exit path does not drain. Main thread | Producer AE 32031 (0x1405195d0), from cell attach AE 18976 (`CellLoaderTask`), TES grid functions and AE 32030: takes a pool entry and **loops on `Sleep` until one is free**; also sets water data +0xB8 and +0x88 flags | After the pool is used up (128 entries, from the destructor), the cell loader hangs | Texture yes; pool no | nothing | CS drains the ring and frees entries on the game thread; the tile copies move to the render side | verified (spin loop rechecked); pool size inferred |
| S20. `TESWaterSystem+0xB8` (player underwater) reset; mirror 0x14338ca68; underwater loop sound | RenderWaterEffects early exit (waterless interior, Map Menu closed) sets +0xB8 = 0. `TESWaterSystem::Update` AE 32174 / SE ? with water disabled calls AE 32061 (stops the underwater sound and the 0x9A audio effect) and clears +0xB8 | The owner is game-side: `PlayerCharacter::ProcessInWater` via AE 32216 / SE 31409. The mirror is read only by the water shader | +0xB8 and the underwater audio could stay on after leaving water into a waterless interior | No (audio) | nothing | CS repeats both resets on the game thread | writes verified; staleness inferred |
| S21. Water ripples (0x1433dcf28..0x1433dcf50), water-object lists +0x20/+0x38/+0x50/+0x68 | `TESWaterSystem::AddRipple` (game) appends; AE 32176 drains (inferred). AE 32174 releases list entries via AE 70544, 32277, 32278 | none | arrays and lists grow (memory) | Yes | nothing | CS drains and collects (game or render side) | ripple drain inferred; collection verified |
| `partiallyUnderwater` +0xB9, `underwaterHeight` +0xC0, water object +0x70 visible bit | AE 32174 from the camera frustum | none found (global mirrors only checked) | unknown | probably | nothing | open | inferred |
| `TESWaterReflections::Update` AE 32160 / SE 31373 cube camera (+0x28), counter +0x4C | AE 32174 | none: hides player and water nodes only for the faces and restores them | none | Yes | — | Goes away | verified |

## 7. Sky and sun

| Write | Writer | Readers outside the render half | Effect if stopped | Renderer-only? | CS today | Suggested fate | Confidence |
|---|---|---|---|---|---|---|---|
| S19. Sun accumulator +0x68 (glare factor; also +0x60, +0x78..); Sun +0x4C `doOcclusionTests` | `Sun::UpdateOcclusionQueries` AE 26355 / SE 25801 polls the ring, from RenderWorld. Queries submitted by AE 26353 / SE 25799 and AE 106604 / SE 99958. AE 107143 clears +0x4C via AE 26354 | `Sun::Update` AE 26352 (run by Sky::Update) via `Sun::GetSunOcclusionPercent` AE 26347 / SE 25793: sets `glareScale` (+0x48), the glare node's scale and hidden bit | glare billboard freezes; visual only | Effectively (a game-thread reader, but only for a scene node) | `SkyHooks.cpp:466-522` answers the ring, but AE 26355 runs inside RenderWorld, so native never polls | CS writes +0x68 from its readback at publish, or drops the glare | verified |
| S22. Post-render job stage: "Face morphing", "Sky" (AE 39087 to 36584 to `Sky::Update` AE 26229 / SE 25682), "Shared particles", "Update grass" (table 0x142011d60) | Released by the stage events DrawFrame signals (S1) | Weather, time of day, `Sun::Update`, precipitation, face morphs | could stop with S1 | No | nothing | Part of S1 | job table verified; event link inferred |
| AE 36561 other writes: sky nodes moved to the camera, player position globals, render view data | AE 36561 | none found | — | Yes | — | Goes away | verified |

## 8. Decals and facegen

| Write | Writer | Readers outside the render half | Effect if stopped | Renderer-only? | CS today | Suggested fate | Confidence |
|---|---|---|---|---|---|---|---|
| S17. BGSDecalManager `+0x60` pending-test array; decal state `+0x50` 3 to 4; query slot `+0x38` | DrawFrame +0x50..0xC7: `Main::DecalTest` AE 36562 / SE 35563 per state-3 decal, then clears `+0x60`. Main thread | AE 15220 (0x1401faca0), from BGSDecalManager AE 15207 in Main::Update: polls AE 30117 / SE 29262 (count to +0x40); 0 drops (state 7), else places (state 5). Each pass stops after `iMaxDecalsPerFrame` | Decals stay in state 3; once enough are stuck at the head, no decal is processed again; `+0x60` grows and holds references | Placement visual, but the queue is game-side | `DecalOcclusion/DecalOcclusionCapture.cpp:164-243` detours AE 36562 (graph), but only when DrawFrame calls it; a skipped test answers 1 | Needs the user's call: test from a snapshot and write +0x38 back, or move state-3 decals straight to placement on the game thread | verified |
| S18. Facegen tint bake queue (0x14313fa50, count +0x60) | `FaceGen::UpdatePending` AE 27041 / SE 26455 (DrawFrame +0x162, and the loading loop): renders ImageSpace effect 99 into each job's texture, frees the job | Enqueued by AE 27040 (from `BSFaceGenManager::PrepareHeadPartForShaders`), AE 40699, AE 52396 (RaceSexMenu). Nothing waits on the result | Face tints never fill; a 0x288-byte job and a reference leak per head | Yes (result) | nothing | CS drains the queue per snapshot and bakes on the render thread | verified |

## 9. Saves, screenshots and UI

| Write | Writer | Readers outside the render half | Effect if stopped | Renderer-only? | CS today | Suggested fate | Confidence |
|---|---|---|---|---|---|---|---|
| S15. ImageSpaceManager +0x211 (screenshot request), +0x218 (capture frame), globals 0x143199390 / 0x143199394 | AE 36859 / SE 35888 (DrawFrame +0x5DA) moves AE 36857's request to +0x211. `RenderEndOfFrameEffects` AE 105674 / SE 99023 copies the frame into `kSAVE_GAME_SCREENSHOT`, clears +0x211, sets +0x218. DrawFrame also requests one whenever a menu is open (+0x5CC). Main thread | Save request pump AE 35772 (Main::Update): for requests with flag bit 29 while Mist Menu is closed, if AE 36858 (ready) is false it calls 36857 and re-queues. Also SleepWaitMenu and AE 52712 | Those saves are re-queued every frame and never written | No | read only: `PostChain.cpp:78,313` | CS produces it: flag logic on the game thread, capture on the render side, set +0x218 when done | verified (writer rechecked); which saves set bit 29 is inferred (autosave/quicksave) |
| S23a. `PlayerCamera::Update` AE 50784 / SE 49852 | `TweenMenu::PostDisplay` AE 52706, called by `MenuManager::DrawInterface` AE 82084 | the game camera | camera does not update while TweenMenu is open | No | nothing | CS calls it on the game thread while TweenMenu is open | call verified; effect inferred |
| S23b. Inventory 3D item: model load finish (AE 51776, load state 5), zoom transition +0x150/+0x154 (AE 51777), NiAVObject::Update, 0x142033184 | `Inventory3DManager::Render` AE 51755 / SE 50882 from Barter, Container, Crafting, Gift, Inventory, Magic PostDisplay | menu scene only | item never appears or animates | UI only | draws latched (`MenuScenesCapture.cpp:721-730`); the update still runs in the engine | CS runs the non-draw part on the game thread (user's call) | verified |
| S23c. BookMenu page movie (`ShowPageAtOffset` per page) | `BookMenu::PreDisplay`, called by AE 82085 for menus flagged 0x1000 | the movie | blank pages | UI only | relies on the engine calling it | CS's UI port drives it | verified |
| S23d. UI3DSceneManager scene root update | `UI3DSceneManager::Render` AE 52727 from MistMenu, Lockpicking, Stats, Journal PostDisplay | menu scene | scene roots not updated | UI only | latched | CS's menu-scene port | verified |
| S24. PrintScreen and frame capture: MenuControls +0x10/+0x11 consumed, `iScreenShotIndex`, INI save, HUD message; capture toggle writes 0x1431874b9, 0x143187540, 0x1420129e8, 0x1431cc280 | AE 52210 / SE 51363, AE 52211 / SE 51364, AE 36853 / SE 35882 (saves render target 0x4C) | Game timer AE 36591 reads 0x1431cc280 (fixed delta) | screenshot and capture keys do nothing | Partly | nothing | Needs the user's call | verified |

## 10. Checked, nothing game-visible

- DrawFrame: dynamic resolution (0x140e587f0 / SE 75706, 0x140e589b0 / SE 75708; input always 0); AE 107646 / SE 100856;
  AE 107698 / SE 100909; AE 107502 / SE 100722; frozen background copy AE 107130 / SE 100412; JobListManager::FrameJobs
  AE 35406 / SE 34551; gIsMapMenuOpen 0x14338c658; 0x143284cf0; frame limiter and profiling timers in Renderer::End.
- RenderPlayerView glue: AE 107122, 107131, 107177, 107145, 107147 / SE 100429, AE 76557, 107121, 76815/76816/76819, 16341,
  107157. `Actor::GetMovementController` AE 37792 is `player+0x150`; its vfunc +0x78 returns a bool, nothing is written.
- Culling: `BSCullingProcess::Process` writes only 0x14335bf30, 0x14338c130 and its cull mode (AE 106930); AE 106979
  (blood splatter shader only); AE 106324 (misdecompiled, no global stores); light receivers and `activeLightMask`.
- UI: `MenuManager::DrawInterface` only draws; Scaleform `Advance` runs in `UI::ProcessMessages` before DrawFrame. FaderMenu
  and the fade-done callbacks are not reached from DrawFrame. LocalMapRenderer AE 16335 writes no fog-of-war data.
- Loading loop (`ServingThread::RunLoadingScreen`, 0x1406d2120): under Renderer::Lock it runs only DrawInterface,
  FaceGen::UpdatePending (S18) and Renderer::End (S2). Its game work is outside the lock. The Bink tick (AE 90259) tracks
  its own progress; CS calls through (`BinkHooks.cpp:241-263`).
- Water: camera underwater 0x142033188 and blend 0x1420331a4; AE 32269, 32225, 32215, 32219 ImageSpace toggles; water
  normals AE 32170; AE 32189.
- ImageSpace AE 105676 / SE 99025, 105675 / SE 99024; VolumetricLighting AE 107014 / SE 100297, 107023 / SE 100306;
  `Main::RenderSaoCs` AE 107252 / SE 100526; precipitation SetupMask AE 26183 / SE 25641, RenderMask AE 26184 / SE 25642;
  screen blood (draws only). Eye adaptation stays on the GPU (inferred). ImageSpace modifier timers advance in Main::Update
  (inferred).
- Papyrus has no `IsOnScreen`. `PlayerCharacter::HasLOS` (SE 39444) uses the camera frustum and `TES::Pick`, not cull
  results.

## 11. CS writes into engine objects from the render path today

The phase 2 rule is "no render-side writes into engine objects". DCLF runs only in graph modes, so its rows apply to guest
and native alike; no native-only runtime write was found. "Graph" = guest and native; "all" = graph and legacy D3D11.
Paths are under `src/`.

| File:line | What is written | Runs from | Mode | Why |
|---|---|---|---|---|
| `Features/DrawcallLimitFix/Core/Scene/FrameView/FrameRequests.cpp:632-641` | NiSwitchNode child revision; child UpdateDownwardPass | FrameDriver::BeginFrame (Main_Draw_Early) | graph | switch catch-up the skipped OnVisible would have done |
| `.../Features/Lod/LodGates.cpp:212-215, 556-560, 1276-1286` | `kHidden` on LOD blocks | BeginFrame, LOD gate adoption | graph | LOD swap in step with the published snapshot |
| `.../Core/Handoff/PrimaryCull.cpp:443` | `lightData activeLightMask = 0` | BeginFrame, install claims | graph | stale shadow-light bits |
| `.../Core/Handoff/PrimaryCull.cpp:985-993` | engine Process1 / AppendVirtual / OnVisible calls | Process1 hook on list cull jobs | graph | stands in for the engine cull |
| `.../Core/Handoff/SceneLists.cpp:395-408, 572-581, 591` | engine scene lists; `kAccumulated` on object-root children | BuildSceneLists detour (scene-list job) | graph | DCLF builds the cull's input lists |
| `.../Features/TreeWind/TreeAnimation.cpp:250-265` | tree manager animation array (+0x50) | AfterFullFrustum, PrepareFrame | graph | stood-in trees off the engine list |
| `.../Features/Shadows/Sun/SunAccumulation.cpp:151-152, 338-339, 427, 499-504` | `activeLightMask`; `kListCullJobs` nulled and restored | registration thunks, RenderShadowMaps, FullFrustum thunk | graph | sun cascade bits; inline empty culls |
| `.../Features/Shadows/Local/LocalLightCull.cpp:567-568, 828, 848` | accumulator light index and bits; `activeLightMask` | light Accumulate; main registration jobs | graph | local-light bookkeeping |
| `.../Features/Shadows/Engine/ShadowMapsNative.cpp:195-258` | `sceneAccumArray`, descriptors, free mask, `lightTransform`; parabola globals, current camera and accumulator, render mode, `SetCameraVectors` | LightRender vfunc in RenderShadowMaps | graph | replays Render's bookkeeping without drawing |
| `.../Features/Sky/SkyHooks.cpp:489-522` | accumulator query ring entries; CreateQuery/Begin/End; `SetCameraData` (:508) | sun occlusion query detour | graph | sun query bookkeeping |
| `.../Features/Sky/SkyNative.cpp:1052-1106`; `.../Features/Water/WaterNative.cpp:576-607` | geometry group pass lists, batch group heads and bits (detach, reattach) | sky, cloud and water batch hooks | graph | hide passes from the engine draw |
| `.../Features/ShadowMask/ShadowMaskHooks.cpp:51-70` | SetDepthTarget/SetRenderTarget, `ReturnShadowmaps`, blend mode, clear colours | mask batch hooks | graph | mask bookkeeping |
| `.../DrawcallLimitFix.cpp:467-470, 493-504` | precipitation occlusion projection, accumulator camera, `posAdjust`/`previousPosAdjust`/`cameraData`, DR lock (saved, restored) | RenderShadowMaps, early precipitation mask | graph | precipitation mask |
| `.../DrawcallLimitFix.cpp:542` and family/epoch `ApplyPendingState` sites | renderer state tracker | depth, opaque, first-person, family hooks | graph | state resync |
| `RenderGraph/Frame/PostChain.cpp:256-267` | State jitter index (+0xC0), `projectionPosScale` | Upscaling `Main_UpdateJitter` (Renderer::Begin) | graph | CS owns jitter |
| `Features/Upscaling.cpp:1429-1450, 1584-1597, 1859-1863, 2177-2188`; `Utils/Game.cpp:219`; `Hooks.cpp:1165-1167` | `projectionPosScale`, DR ratios and lock, viewport, camera data; ISTemporalAA `taaEnabled` | Begin, DrawInterface, PostProcessing, precipitation and facegen hooks | all | jitter, render scale, TAA toggle |
| `State.cpp:455, 464` | `ApplyReflections->active`; INI `bEnableImprovedSnow` | State::Reset after Present | all | per-frame overrides |
| `Features/SubsurfaceScattering.cpp:423, 428`; `EngineFixes/LODBlendRingFix.cpp:88-90` | shadow state character light; grid array centre and lerp | Feature::Reset / State::Reset after Present | all | per-frame state |
| `Features/Skylighting.cpp:715-845` | precipitation depth-stencil entry, cube globals, occlusion camera, SetupMask/RenderMask, accumulator camera, DR lock | RenderShadowMaps hook | all | sky occlusion map |
| `TerrainBlending.cpp:383` | `SetCameraData(WorldRootCamera)` | Main_RenderDepth | all | main camera for depth |
| `EngineFixes/SunShadowMaskFix.cpp:62`; `TruePBR.cpp:1015, 1799`; `EngineFixes/LandscapeSeamsFix.cpp:710` | `activeLightMask`; `BSRenderPass passEnum` | GetRenderPasses vfuncs (registration jobs) | all | missed sun bits; technique remap |
| `Features/LightLimitFix.cpp:637`; `CSEditor/LightEditor.cpp:2335-2370` | NiLight runtime data (and editor overrides: colour, fade, radius) | LLF prepass, SetupGeometry | all | light extension data lives in the NiLight; **game reads NiLight fields** (stealth luminance) |
| `Features/VolumetricLighting.cpp:233-235`; `Features/UnifiedWater.cpp:834-849`; `Features/ScreenSpaceGI.cpp:818`; `Features/PostProcessing.cpp:1078, 1081` | VL globals and quality; flow-map globals and water property flow/cell; ISSAO enable; INI `bLensFlare` | prepass, water SetupGeometry, SSGI draw, vanilla effects update | all | engine settings follow CS |
| `Menu.cpp:1491, 1495`; `CSEditor/EditorWindow.cpp:1898-2364` | INI `fAutoVanityModeDelay`; Sky weather override, weather and lighting-template form data | overlay and editor at Present | all | UI and editor edits (game-visible) |
| `Deferred.cpp`, `ImageSpace.cpp`, `Tripwire.cpp:2539-2543`, `Hooks.cpp:291-296, 1305-1345` and feature render hooks (list in snap0-effects.md) | renderer state tracker (targets, modes, current shaders) | render hooks | all | resync after CS binds |

Game-thread writes for comparison (all at `Main_Update_Entry`, `DrawcallLimitFix.cpp:313-317`): `Visibility.cpp:651-724`
(`kAccumulated`), `FadeWriteBack.cpp:74-83`, `ActorRate.cpp:98`, `TreeAnimation.cpp:313, 334`. SkySync, Lightning,
Effects11, ProceduralAurora, WeatherTransition and UnifiedWater also write sky and water objects at Sky::Update and
TESWaterSystem update hooks (game thread).

Of these render-path writes, the ones game code reads are: the light-mask and light-list edits (stealth reads per-geometry
lists, S13), `LightLimitFix` / `LightEditor` NiLight fields, the editor's weather and form edits, the INI writes, the tree
manager list, and `kAccumulated` from `SceneLists.cpp`. The rest are renderer state that goes away with the engine renderer.
