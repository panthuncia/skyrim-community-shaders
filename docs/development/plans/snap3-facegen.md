# snap3-facegen: face tint bakes without the engine, and the frame skeleton's inventory (phase 3)

Lead: `facegen`, branch `snap3/facegen` (base `org_integration` 175c008da), stage `s7-facegen`. Phase 3 of
[render-thread-snapshots.md](render-thread-snapshots.md) ("game-visible side effects (... facegen) produced by CS on the game
thread"), side effect S18 of [render-side-effects.md](render-side-effects.md), decisions 8, 70 and 127 of
[render-thread-decisions.md](render-thread-decisions.md).

## 1. The two DrawFrame render calls

### FaceGen::UpdatePending (SE 26455, AE 27041)

What it does (Ghidra, AE, SE matches): under the queue's spin lock (AE 0x14313fa68 / SE 0x142f07d68, ids 401752 / 515577), for
each job in the tint queue (BSTArray at AE 0x14313fa50 / SE 0x142f07d50, ids 401749 / 515574):

1. sets effect 99's (ISAlphaBlend) textures 1-16 to the job's sixteen ImageSpaceTextures (the masks; FUN_1414e80c0);
2. `ImageSpaceManager::RenderEffect(99, kPLAYER_FACEGEN_TINT, &job+0x200)`: the masks blended over grey with the colours in the
   job's ImageSpaceShaderParam;
3. if the job has a head texture (`job+0x280`, an NiSourceTexture): releases its old renderer texture (77322) and gives it a
   copy of kPLAYER_FACEGEN_TINT (77318, CreateTexture2D plus CopyResource; CS's Upscaling thunks that copy);
4. frees the job (its head reference, the param, the masks, the 0x288 bytes), then empties the queue.

Jobs are queued by AE 27040 (`BSFaceGenManager::PrepareHeadPartForShaders`, the player's head at a load, the race menu, actors
without pre-built facegen tints). Nothing waits on the result. It runs every frame, queue or not (DrawFrame +0x162, and the
loading loop AE 39039 +0x19F), so it counted one render call in every world frame.

The split (decision 8):

- **Game half** (queue drain, job lifetime, handing the texture to the head) runs in the snapshot capture,
  `Snapshot/FaceTint.cpp` `CaptureBlock`. Each job becomes a `Bake`: EngineRefs to its masks with their views, the ISAlphaBlend
  rows built from its param (`ImageSpace::BuildTintRows`, the generic SetupGeometry's rows), an EngineRef to the head, and a new
  texture for the result made as the engine's copy makes it (CreateTexture2D, kPLAYER_FACEGEN_TINT's size, RGBA8, DEFAULT).
  The job is then freed as UpdatePending frees it. Bakes stay in the `FaceTint` block until the render side reports them done;
  the next capture then hands each texture to its head and releases the old one.
- **Render half**, `FaceTint::RenderFrame`, called at the end of the Renderer::Begin detour, where DrawFrame called
  UpdatePending. In a world frame it stages one bake as the face tint step (`ImageSpace::StageFaceTint`: the existing
  `cs.engine.imagespace.facetint` client in the start stage, ISAlphaBlend into kPLAYER_FACEGEN_TINT, with the bake's masks and
  rows instead of the effect's texture list and the engine's call). The next world frame copies kPLAYER_FACEGEN_TINT into the
  bake's texture (the native context, after the earlier frame's start stage was submitted) and reports it done. One bake at a
  time, since bakes share kPLAYER_FACEGEN_TINT.
- Both engine calls are NOPed in native (bytes checked, fatal on a mismatch). Guest and D3D11 are unchanged.

Bakes are keyed by serial: a repeated snapshot stages nothing new, a dropped snapshot loses nothing (its bakes are still in the
next block until reported done).

Found on the way: the engine path in native was ordered wrong. The step was staged into the start stage, which records at its
commit, while the engine's copy ran through the native context at once, ahead of it (BeforeGpuWork with no segment commits
nothing). So a bake's texture got kPLAYER_FACEGEN_TINT's previous contents. Copying a frame later fixes that.

### JobListManager::FrameJobs (SE 34551, AE 35406)

`return ServingThread && (a == 0 ? state != 0 : state == a)` on `JobListManager::ServingThread +0x6c`. DrawFrame calls it with 1
and, when true, waits for the UI job's stage (`WaitForJobTask(0, 2)`). It reads game state and writes nothing: no rendering.
**Reclassified as a helper** in `EngineZones.cpp` (the generator's list, `perf-tools/gensites2.py`, still names 35406 a render
site). Phase 7: the wait belongs with DrawInterface on the game side (row below).

## 2. Inventory: every call in a native world frame's skeleton (phase 7)

What each call does, what kind of work it is, and what CS does with it once it drives the frame itself (decision 70,
snap7-prep's plan: the game thread only captures, runs side effects and DrawInterface; the render thread runs CS's frame).
"Detector" is the native-tour count per world frame before this branch. Addresses are AE; SE ids are in `EngineZones.cpp`.

### Main::DrawFrame (AE 36555)

| Call (offset) | Detector | What it does | Kind | Phase 7 |
|---|---|---|---|---|
| dynamic resolution (+0x2D) | n/a | NOPed by Upscaling | - | already gone |
| Renderer::Lock (+0x39, 77243) | 1.00 helper | EnterCriticalSection(renderer+0x27F0): serialises the main thread's frame with the loading loop's | bookkeeping | CS's render thread owns rendering (ClaimRenderOwnership); nothing calls it in native |
| Renderer::Begin (+0x47, 77245) | 1.00 helper | applies a pending window resize (WindowSizeChanged), resets the State and shadow state, binds the default samplers, picks the render window, sizes the target manager. CS's detour runs Adopt, OnBegin and now the face tint render half | rendering bookkeeping | CS's frame start; the resize is the snapshot event (snap7-prep 6) |
| Main::DecalTest loop (+0xC7, 36562) | 0 | empty in native (S17, SideEffects) | - | already gone |
| FaceGen::UpdatePending (+0x162, 27041) | 1.00 render | above | mixed | **0: NOPed; game half in the capture, render half in CS's frame start (this branch)** |
| AE 82085 (+0x1CD) | 1.00 helper | under the menu lock, each open menu flagged 0x1000 runs its vfunc +0x38 (PreDisplay: BookMenu's pages, S23c) with the Scaleform lock; also latches the target manager's current target (DAT_14328cbfc) | UI game logic | game side, with DrawInterface in the publish (decision 16) |
| AE 107698 (+0x1E5) | 1.00 helper | under its lock, rotates the engine shaders' per-frame cache (0x143187760, read by BSLightingShader's setup and UI3DSceneManager::Render): moves queued entries into its map, frees entries with a D3D object at +8 | engine renderer bookkeeping | goes away with the engine's shader setup; while menu scenes still run engine setup (phase 5), call once per frame wherever they run |
| AE 107646 (+0x1EA) | 1.00 helper | clears 0x1435e9fd0 (an engine shader state word) | engine renderer bookkeeping | goes away |
| AE 52210 (+0x207) | 1.00 helper | MenuControls: PrintScreen pressed (+0x10), consumed | game input (S24) | game side (decision 15) |
| Main::FreezeTest #1 (+0x217, 82075) | 1.00 helper, hooked | under the menu lock: any open menu with flag 0x20 (freeze background) | game state read | the capture's `menus.freezesBackground` (already); nothing calls it |
| Main::RenderScene #2 (+0x4C8, 36558) | 1.00 render | the world branch (table below) | rendering | CS's frame |
| AE 51639 (+0x4BE) | 1.00 helper | under its lock, copies the world camera's ShadowSceneNode light lists (+0x110..+0x148) and fog values (+0x174..+0x180) into the shader globals 0x1431ad3a0 / 0x1431ae960 and sets 0x1431ad391 | engine renderer bookkeeping | goes away with the engine's lighting shaders (DCLF reads the snapshot) |
| SetJobTaskEvent 6/7 (+0x2E0/+0x2EA, menu branch) | n/a | S1 | game | already CS's (SideEffects) |
| AE 107130 (+0x2C3, menu branch) | 0 | the frozen background copy | rendering | phase 5 (frozen kind) |
| ImageSpaceManager::RenderEffect #1 (+0x329) | 0 in world | the map's or capture's effect (menu branch) | rendering | phase 5 |
| JobListManager::FrameJobs (+0x333, 35406) | 1.00 render | above | game state read | **helper (this branch)**; with WaitForJobTask on the game side |
| WaitForJobTask (+0x343, 69516) | 0.40 helper | waits on job stage 2 (the UI job) when FrameJobs(1) | game sync | game side, before DrawInterface in the publish |
| MenuManager::DrawInterface (+0x371, 82084) | 1.00 producer | UI | UI producer | game side (decision 16) |
| AE 36853 (+0x454, menu branch) | 0 | screenshot file and HUD message | game + capture | decision 15 |
| AE 52211 (+0x50B) | 1.00 helper | MenuControls: capture toggle key (+0x11), consumed | game input (S24) | game side (decision 15) |
| Main::FreezeTest #2 (+0x5C3) | 1.00 helper | as #1, for the screenshot request below | game state read | the snapshot's menus |
| AE 36858 / 36857 (+0x5CC/+0x5D5) | menu frames | screenshot ready / request (globals 0x143199394 / 0x143199390) | game (S15) | game side |
| AE 36859 (+0x5DA) | 1.00 helper | moves the screenshot request into ImageSpaceManager +0x211/+0x218 when no menu is open | game (S15) | game side; the capture reads it (snap5-isrest) |
| SetJobTaskEvent 8 (+0x5E4) | n/a | S1 | game | already CS's |
| BSSceneGraph sub (+0x5F0) | n/a | world root per-frame update (not in the detector table) | to check | to check in phase 7 |
| AE 107502 (+0x5F5) | 1.00 helper | drains a lock-free queue (0x14339ccb0) of pooled render objects back to their per-pool free lists (0x14338cc70, stride 0x28) | engine renderer bookkeeping | goes away with the engine's pools; keep while engine code fills the queue (menu scenes) |
| Renderer::End (+0x601, 77246) | 1.00 helper | frame limiter, the render window's Present (vtable +0x40), `++gFrameCounter` (NOPed, S2), clears 0x14328cc74 | rendering + pacing | CS's present on the render thread; pacing is PaceGameUpdate (snap7-prep 4) |
| Renderer::Unlock (+0x60D) | n/a | LeaveCriticalSection | bookkeeping | goes away with Lock |

### Main::RenderScene (AE 36558)

| Call | Detector | What it does | Kind | Phase 7 |
|---|---|---|---|---|
| (inline) | n/a | sets 0x143284cf0, viewport from 0x14332a470, copies the world camera's position into BSShaderManager::State +0x2F4 and the main accumulator +0x16C | rendering bookkeeping | goes away (CS's frame camera) |
| AE 107122 (+0xF5) | 1.00 helper | copies DrawWorld's eye position (0x14338c8c0) into the main accumulator +0xA0 | rendering bookkeeping | goes away |
| AE 82074 (+0x10F) | 1.00 helper | UI: is the menu named InterfaceStrings+0xA8 open with flag 0x40 (passed to RenderPlayerView as "no world") | game state read | the snapshot's menus |
| Main::RenderPlayerView (+0x11D, 36559) | 1.00 render | below | rendering | CS's frame |

### Main::RenderPlayerView (AE 36559)

| Call | Detector | What it does | Kind | Phase 7 |
|---|---|---|---|---|
| Main::SetupSunAndSky (+0xD3, 36561) | 1.00 render, hooked | camera and sky setup, the high actor culler (S6, skipped) | mixed | snap3-water's lead |
| AE 76557 (+0x11B, per culling process) | 6.00 helper | resets each list culling process: releases its +0x128 references and rewinds its page lists (+0x301D8) | engine cull bookkeeping | goes away: the list culls no longer run (snap3-culls), so the reset is provably redundant; left in place (inside another lead's skeleton) |
| Main::RenderWaterEffects (+0x172) | 1.00 render | water | rendering | snap3-water |
| AE 106337 (+0x194) | 1.00 helper | ShadowSceneNode::ProcessAllQueuedLights for the four nodes: applies the add/remove light queues and the renderer's light-attach walk | mixed: the lists are game-visible (S12/S13), the walk feeds the renderer | game side in the publish (decision 8): the queues are game state. StealthLights reads the membership it would leave |
| Main::CalculateAndDrawShadowCasterLights (+0x1C4) | 1.00 render, hooked | shadow casters | rendering | CS (NativeCasters) |
| Main::FirstPersonCulling (+0x1F2) | 0 | skipped (snap3-culls) | - | gone |
| AE 107123 (+0x26A) | first person only | first-person accumulator setup (camera, eye, render mode) | rendering bookkeeping | goes away |
| SetCameraData #1/#2/#3 (+0x2E7, +0x30A, +0x3B6) | 1.00 / 0.65 / 1.00 helper | BSGraphics::State::SetCameraData(world camera): finds or makes the camera's cached matrices and makes them current | rendering bookkeeping | goes away: CS's frame camera from the snapshot (FrameCamera) |
| Main::RenderShadowMaps (+0x2EC) | 1.00 render, hooked | | rendering | CS |
| AE 107121 (+0x31D) | 0.65 helper | returns DrawWorld | read | goes away |
| VolumetricLighting::Descriptor / Render (+0x325/+0x335) | 0.65 helper / render | | rendering | snap3-post |
| AE 82074 (+0x34F) | 1.00 helper | UI: is the map menu (InterfaceStrings+0x110) open, into gIsMapMenuOpen | game state read | the snapshot's menus.map |
| Actor::GetMovementController #1/#2 (+0x361/+0x373) | 1.00 helper | `player + 0x150`; its vfunc +0x78 (a bool, no write) picks the first-person mode passed on | game state read | capture it in the snapshot (first-person flag) |
| Main::RenderDepth / RenderShadowMasks / RenderPrecipitation | 1.00 render | | rendering | snap3-depth, CS |
| AE 107018 (+0x3C6) | 0.65 helper | an empty function (`ret`) called when VL is on | none | provably dead; harmless; left (VL is snap3-post's) |
| AE 107131 (+0x645) | 1.00 helper | sets render target 0 to kMAIN and the depth target for the world | rendering bookkeeping | goes away |
| AE 107177 #1/#2 (+0x6B4/+0x730) | 1.00 helper | the normals target id: `4 | parity of gFrameCounter` when SSR is on, else 4 | read | goes away (CS's targets) |
| JobList::Finish #2 | 0 | skipped | - | gone |
| ShaderAccumulator::GroupFiveCount (+0x870, 106598) | 1.00 helper, hooked | the main accumulator's group 5 count | read | goes away |
| ShaderAccumulator::ClearGroup #1/#2 (+0x88C/+0x89D, 106600) | 1.00 helper | clears the accumulator's groups 6 and 5 when group 5 is empty | rendering bookkeeping | goes away |
| AE 107145 (+0x879) | when group 5 has passes | renders group 5 for the world camera | rendering | goes away (nothing registers in native) |
| Main::ClearMasks / RenderFirstPersonView / AE 107124 | first person only | first-person draw and its eye restore (107124 sets the accumulator eye and BSShaderManager::State +0x2E8..+0x2FC, render mode 0) | rendering | CS |
| AE 39034 (+0xA0F) | 1.00 helper, hooked | the render-unsafe thread mask (S4) | game-visible | already CS's (SideEffects, thunked to nothing) |
| Main::RenderSaoCs / RenderPostProcess | 0 / 1.00 render | | rendering | snap3-post |
| AE 107147 (+0xA73) | 0.94-0.96 helper, hooked | screen splatter: SetCameraData(splatter camera), target 0, renders the splatter accumulator (FUN_1414b4b00) | rendering | CS (screen blood, snap3-accum) |
| AE 107143 (+0xA78) | 1.00 helper | the tail: Sun +0x4C `doOcclusionTests = 0` (S19), clears the main accumulator, releases an ImageSpaceTexture (0x142033da8) when set, releases 0x1435e9fd8, the fade enable count (S5, CS's) | mixed | the sun flag and fade count on the game side (S5 is already); the rest goes away |

Nothing here was removed as dead: AE 107018 and AE 76557 are provably without effect in native (an empty function; resets of list
processes nothing fills), but both sit in other leads' paths (VL, the culls' skeleton) and cost nothing. Decided by the agent,
to confirm.

## Runs (stage `s7-facegen`, Dev-Fast, commit 7b122a22a)

| Config | Label | Result |
|---|---|---|
| native-vulkan+facegen (detector) | s7-facegen-a-nvk-facegen | passed; bakes 1-4 queued and handed to the head (load, race menu) |
| guest+facegen | s7-facegen-a-guest-facegen | passed (reference; guest unchanged) |
| native-vulkan+facegen, engine path (a local build with this branch's install off, not committed) | s7-facegen-b-nvk-facegen-enginepath | passed; the same purple noise on the player's face as with this branch |
| guest+quick | s7-facegen-f-guest-quick | passed |
| native-vulkan+quick (detector) | s7-facegen-f-nvk-quick | passed; 34.8 render calls a world frame (167 frames, mostly load) |
| native-d3d12-quick | s7-facegen-f-nd12-quick | passed; bakes handed over |
| native-vulkan+native-interior (detector) | s7-facegen-f-nvk-interior | passed; 17.8 |
| native-vulkan+native-tour (detector) | s7-facegen-f-nvk-tour | passed; 18.6 (base s3-accum-e-tour 20.0) |
| native-vulkan+cadence-drop-cells | s7-facegen-f-nvk-dropcells | passed; bakes handed over under drop:3 |

Detector: FaceGen::UpdatePending in DrawFrame 1.00 to 0 (the site is gone: "no CALL ... skipped"), in the loading loop 1.0 per
loading frame to 0; FrameJobs 1.00 render to 1.00 helper. Native tour 20.0 to 18.6 a world frame.

## Decisions (agent, to confirm)

- **FG-a.** The loading loop's UpdatePending is NOPed too: it drains the same queue, and the player's head is queued during loads.
  Bakes then wait for the first world frame after the load.
- **FG-b.** One bake a render frame, copied the next frame. A head shows its old texture for two or three updates more than
  vanilla (behind the load hold at a load; a few frames in the race menu).
- **FG-c.** A bake's texture is made on the game thread with the engine's CreateTexture2D (resource creation, as the engine's
  loader threads do), and handed to the head only after the copy. Vanilla hands it over in the same frame.
- **FG-d.** FrameJobs reclassified as a helper (it only reads state). The generator's list `perf-tools/gensites2.py` still names
  35406 a render site; it should drop it before the table is generated again.
- **FG-e.** No dead call was removed (AE 107018, AE 76557; table above).

## Open items

- **Purple noise on the player's face in native, before and after this branch** (the engine-path A/B run shows the same). So it is
  not the bake ordering. Suspects: the mask textures' bindings (texture streaming shells read before their window is in), or the
  face material's draw in DCLF. A RenderDoc capture of the face tint step's inputs is the next step. Guest's race menu shot
  is dark under the helmet and has a tutorial popup over it, so it is not a clean reference.
- NPC faces: the Dragonsreach shots show no NPC close up (a tutorial popup and blur cover the frame), so they were not checked
  against a reference. Vanilla NPCs use pre-built tint textures and queue no bakes, so this branch does not touch them.
- The loading frames' detector drops by one render call (the loading loop's UpdatePending); snap5-kinds listed it as left.
