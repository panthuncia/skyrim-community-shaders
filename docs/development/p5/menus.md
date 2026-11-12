# P5.8 and P5.9: the menu scenes as DCLF views

Plan tasks P5.8 (menu 3D) and P5.9 (map menu) ([org-full-port.md](../org-full-port.md), Phase 5). Reverse engineered with GhidraMCP on
SE 1.5.97 and AE 1.6.1170 (2026-11-10). IDs are Address Library IDs. The code is `src/Features/DrawcallLimitFix/Features/MenuViews/` and
the switches are `CS_ORG_MENU3D=1` and `CS_ORG_MAP=1` (default off).

**State (2026-11-12, Phase 5.5.7):** the menu scenes are an engine draw family, as the water and the shadow masks are
(Core/Engine/Family), not DCLF views. The local map's Utility passes are drawn on the graph with `CS_ORG_MAP=1` (MenuScenes*.cpp,
below). Menu 3D is still the engine's under its vouch: it needs Lighting and Effect programs (and Utility's depth and shadow map
techniques) that no family has yet. The probe (stage 1) stays as a diagnostic.

**Why not DCLF views.** Every census draw under the menu 3D vouch is the loading-screen and main-menu model (scheme 4). It renders on
the serving thread during a load and at the main menu, where the probe logs `DCLF's scene has no publication`: DCLF's tables are frozen
in a load, so a DCLF view has no members there. A menu model is a handful of draws, so latching the engine's draws costs little. The
`ID 84959`/`84961` draws the census shows in the same windows are not menu 3D: they are Scaleform (`GFxMovieRoot::Display`, AE 83789,
from the shared menu PostDisplay, AE 53317, which calls `uiMovie->Display()` before `UI3DSceneManager::Render`), outside the vouch's
scope and Scaleform's (5.5.8, a movie into kMAIN).

## RE

### Which engine renders are menu scenes

The callers of `Renderer::SubmitAccumulator` (SE 99789 `0x1412c15c0`, AE 106436 `0x1414a90f0`) are every scene render the engine
makes (R9). Checked again on SE: Precipitation::RenderMask, the decal occlusion pass (35563), `Main::RenderFirstPersonView` (twice),
`Main::RenderDepth`, `Main::RenderWorld`, `LocalMapRenderer::Render` (twice), `UI3DSceneManager::Render`, `BSCubeMapCamera::RenderFaces`,
`BSShadowLight::RenderShadowmap`. So the menu scenes are two functions, and nothing else:

| Scene | Renders through | DCLF today |
| --- | --- | --- |
| Inventory, barter, container, gift, magic, crafting item previews | `Inventory3DManager::Render` (SE 50882 `0x140887750`, AE 51755 `0x140927560`) -> `UI3DSceneManager::Render(mgr, this+0x34, NULL, 0)` | not claimed |
| Journal book, book, stats constellations, lockpicking, main menu and loading-screen model | `JournalMenu`, `BookMenu`, `StatsMenu`, `LockpickingMenu`, `MistMenu` PostDisplay -> `UI3DSceneManager::Render` | not claimed |
| Local map | `LocalMapRenderer::Render` (SE 16094 `0x1401f5b10`, AE 16335 `0x140242e00`), from `LocalMapMenu` | not claimed |
| World map | `Main::Draw` with the map camera (no menu render path) | DCLF's main view (GeometryPort writes the WorldMap rows) |
| Race menu | `Main::Draw` with `RaceSexCamera` (a camera state, SE `0x1408af910`..; no menu render path) | DCLF's main view (the player as an actor) |

Renamed in Ghidra: `LocalMapRenderer__Render` (SE and AE), `MistMenu__PostDisplay`, `StatsMenu__PostDisplay`, `BookMenu__PostDisplay`,
`JournalMenu__PostDisplay`, `LockpickingMenu__PostDisplay`, `ImageSpaceFill__DrawToTarget` (SE `0x1412ee4a0`, below).

So the race menu and the world map need no view of their own: their geometry is the main view's. What they need is checked by the
census (`CS_DCLF_CENSUS=1`) and the shots, below.

### Does the world frame run under a menu

Main's frame (SE `0x1405b1020`) calls `MenuManager::sub` (SE `0x140ebe220`): true when an open menu has `kFreezeFrameBackground` (IMenu
flag 0x20). Then the world is not drawn: `FUN_1405b1650` renders it once (`Main::Draw` mode 0xE, while the player has 3D) and
`FUN_1412e2550` composites the frozen frame every frame after. Otherwise `Main::Draw` runs (`FUN_1405b1710`). `UI::Render`
(`UI__DrawInterface`, 79947) runs after either. **DCLF's frame starts in Main::Draw**, so under a freeze-frame menu DCLF has no frame
at all. Which menus freeze is not read here; the probe logs it per render (`world frame no (freeze-frame menu open)`).

### UI3DSceneManager::Render (SE 51855 `0x1408d2390`, AE 52727 `0x140972590`)

`Render(mgr, scheme, camera, keepDepth)`. CommonLib's layout matches both runtimes: +0x08 culling process, +0x10 colour accumulator,
+0x18 depth accumulator, +0x20 camera, +0x30 the menu's `ShadowSceneNode`, +0x38 `menuObjects[8]` (a root per light scheme), +0x78 the
menu lights, +0x90 the current scheme. Schemes (`INTERFACE_LIGHT_SCHEME`): 0 journal, 1 inventory, 2 magic, 3 book, 4 loading, 5 unknown,
6 stats, 7 lockpicking.

In order:
1. Returns unless the scheme is current (or 0) and the UI is up. Takes mgr+0x28's lock.
2. The scheme's light (`menuLights[scheme]+0x28`) goes to the menu's shadow scene node (`FUN_1412babd0`); `NiAVObject::Update` on the root.
3. `SetCurrentDepthStencilTarget(kMAIN, CLEAR)`; `SetPerFrameBuffers` with the menu camera (param, else mgr+0x20, else the world camera);
   the eye into both accumulators (+0x16C); `CacheCameraData`, `UpdateAllPreviousFrameCameraData`, `SetCameraData`. Camera data is
   "TAA on" (`cVar18`) when ImageSpaceManager+0x1F0's +0x18 is set and the scheme is not 3 (book) or 6 (stats).
4. Scheme 4 with a loading model: `FUN_1408d2f80`. With keepDepth: a cull of the root into the depth accumulator (+0x18) and
   `StartAccumulating(cam, acc+0x18, flags | 0x20)` with depth mode 3.
5. A cull of the root into the colour accumulator (+0x10) (`BSCullingProcess::Process`): this is where the passes reach
   `BSBatchRenderer::RegisterPass`, so where DCLF's `PassCapture` would withhold them.
6. **Stencil mode 2, ref 0xFF** (write bit 0, ALWAYS REPLACE; RSS +0x90 / +0x94).
7. With TAA and scheme 1, 2 or 7: `RT1 = kMOTION_VECTOR` cleared to 0 (`ApplyState`).
8. Scheme 4: depth kMAIN (CLEAR, or RESTORE with keepDepth), `RT0 = kMAIN` cleared. Other schemes leave RT0 as `UI::Render` bound it.
9. **`SubmitAccumulator(cam, acc+0x10, flags | 0x200)`** (SE +0x658, AE +0x659): the model's draws. The probe's hook.
10. `FUN_1412cca10(acc, 5)` (AE `0x1414b4ae0`), the accumulator's reset.
11. With TAA: `SetDirtyStates`, **stencil mode 28 (0x1C), ref 0xFF** (NOT_EQUAL on bit 0), then `ImageSpaceFill__DrawToTarget`
    (SE `0x1412ee4a0`, AE `0x1414d8190`; SE +0x710, AE +0x713) on ImageSpaceManager+0x1E0 with the fill (1, 0, 0, 1) into **render
    target 0x55, `kTEMPORAL_AA_MASK`**: a full-screen fill of the TAA mask everywhere the model did not draw. R2's table called 0x55
    an effect; it is a render target index. Then stencil mode 0.
12. With ImageSpaceManager+0xB0 set: the HDR chain (`FUN_141298260`, `FUN_141298350`, `FUN_1412e76f0`,
    `RenderEndOfFrameEffects(1, 0, 0)`); else effect 0x22 (copy) of the chosen target to the framebuffer (per scheme: 0x29 or 1).
13. Depth kMAIN, `ApplyState`, the lock released.

`UI::Render` (SE 79947 `0x140ebf510`): RT0 stays what the frame left (set to kMAIN when none), RT1 = kMOTION_VECTOR, RT2 =
`TAAMASK_SSRMASK_SWAP` (none when RT0 is the framebuffer), depth kMAIN, `ApplyState`, Scaleform begin, each menu's PostDisplay (vfunc
0x30), Scaleform end, effect 0x22 to the framebuffer when RT0 is not it.

**What a menu model's view draws**: the root's geometry, BSLightingShader (and its effects: enchantment glow, BSEffectShader), skinned
or not, lit by the scheme's menu lights through the menu's own shadow scene node (not the world's: Light Limit Fix's clusters are the
main camera's), with the menu camera (perspective), into UI::Render's RT0 and kMOTION_VECTOR, testing and writing kMAIN's depth, and
writing stencil bit 0 for the TAA mask fill that follows.

### LocalMapRenderer::Render (SE 16094 `0x1401f5b10`, AE 16335 `0x140242e00`)

1. `BSShaderManager::State+0x219 = 1`: the local map mode. In it a Lighting property's `GetRenderPasses_LocalMap` (vfunc 0x2C, SE
   `0x1412c7cd0`) makes one BSUtilityShader pass with the Utility technique bits `| 0x2000000` (RENDER_BASE_TEXTURE), and the alpha
   test bit when the alpha property tests. Exceptions: a kLandscape property (flag bit 14) gets its plain `GetRenderPasses` (Lighting's
   land passes), and a skinned or multi-texture property (`flags & 0x8002`), an object under its fade, or one with a hidden alpha gets
   none. In render mode 0x13 the pass is BSUtilityShader's `| 0x10000000` (LOCALMAP_FOGOFWAR).
   Then the accumulator's shader state (+0x148), `BSTreeManager` LOD on (`FUN_1403de280`), clear colour 0.
2. Culls (`BSCullingProcess::Process` on +0x301F8) the cells' nodes, then the LOD roots (BSShaderManager::State+0x118's +0x48 and +0x40)
   with the local map camera (+0x302B8, orthographic).
3. Depth kMAIN CLEAR, `RT0 = kLOCAL_MAP_SWAP (0x11)` CLEAR, **`SubmitAccumulator(cam, acc (+0x302C8), 0)`** (SE +0x340, AE +0x342): the
   land and objects.
4. If `DAT_141db3ec8`: render mode 0x13, the **fog-of-war root** (+0x30358, not the water) culled, depth mode 8, `RT0 = 0x11` RESTORE,
   **`SubmitAccumulator` again** (SE +0x430, AE +0x433), then depth mode 3. The probe names this batch `local map water`; it is the fog.
5. Unbinds depth, sets the fog-of-war parameters (+0x302D0, `FUN_1412d66f0`), **ImageSpace effect 0x62 (`BSImagespaceShaderLocalMap`)
   from 0x11 into kLOCAL_MAP (0x10)**, restores the state. ISLocalMap reads the colour's luminance and edges and the alpha's fog.

No stencil.

## Design, scene by scene

Rules the design keeps: a view is registered at install with a fixed shape, so opening a menu never registers a client or changes a
shape (no graph rebuild); a scene is withheld only when its view claims every geometry of it; anything else the engine draws.

### The probe (written, stage 1)

At each menu scene's SubmitAccumulator (main thread only) `MenuViewsProbe.cpp` reads the targets, depth and stencil mode the engine set,
the camera's projection, whether DCLF's frame ran since the scene's last render (its frame number moved) and whether a freeze-frame menu
is open, and for menu 3D the scheme's root geometry: how many are Lighting, skinned, tracked by DCLF's installed tables and claimed by a
set phase. A change logs one line with a verdict (the first input a view lacks). It never withholds and registers no graph client. It
is a diagnostic: it runs only with `CS_ORG_MENU3D=1` or `CS_ORG_MAP=1`. DCLF's tables are scanned only when the menu root's geometry
changes, or every 300 renders, not at every menu render. The manager's +0x38 and +0x90 are CommonLib's members, checked in Ghidra on
both runtimes (`Render` reads both there).

Expected first results: menu 3D's geometry is not tracked (no view names the menu roots as category roots yet), and the inventory may
have no DCLF frame (if it freezes the world).

### The local map as an engine draw family (written)

`Features/MenuViews/MenuScenes.h`, `MenuScenesCapture.cpp`, `MenuScenesDraws.cpp`, on with `CS_ORG_MAP=1`:
- Thunks on the two SubmitAccumulator calls (step 3 and 4) open a batch (views `local-map` and `local-map-fog`). DrawTriShape (the
  families' one detour) latches each BSUtilityShader draw of the base texture or fog technique, not skinned: the engine constants
  (VS b0, b1, b2; PS b1, b2), the alpha reference, t0 and t7 with their samplers, the geometry by device address, the engine's
  blend, depth-stencil and raster states, the viewport. Then `Engine::ApplyPendingState`, so the render's clears land before the run.
- After the batch the EngineDraws anchor runs the client of its view (`cs.menu-scenes.local-map`, `cs.menu-scenes.local-map-fog`,
  made at setup, orders 5 and 6): one draw a latched draw into kLOCAL_MAP_SWAP with kMAIN's depth, in the engine's order, with
  Utility.hlsl's `UTILITY_ORG_SCENE` build (vertex pulled; programs keyed by the vertex descriptor, as the water's).
- A pass of a shader with no program (watched SetupGeometry of Lighting, Effect and DistantTree: the landscape's Lighting passes, tree
  LOD), a non-TriShape pass, or a refused draw ends the latched part: it runs, and the rest is handed back to the engine unvouched, as every
  family's is (the census names it). So until a Lighting program exists the land and what follows it stay D3D11.
- The ImageSpace effect (step 5) is the ImageSpace port's.

### Menu 3D as an engine draw family (not written)

The same pattern at `UI3DSceneManager::Render`: batches at its SubmitAccumulator calls (the depth pre-pass with keepDepth, the colour
submit at step 9), and its menu lights' shadow maps (`BSShadowLight::RenderShadowmap` inside it). Its shadow masks are already the
shadow mask family's (`menu` view). What it needs that no family has:
- **Lighting.hlsl built for ORG** (the menu models' colour draws, skinned or not; also the local map's land). The work: the cbuffers
  (VS b0-b2, VS_PerFrame b12, PS b0-b2, b11) as latch statics by their packoffsets, t0-t15 and s0-s15 by heap index, the Permutation
  block per draw (as Water's `DCLF_PULLED_ROWS`), the bones (Skinned.hlsli's cbuffer) latched for skinned draws, the vertex pulled
  (Lighting's DCLF pulled stage reads the same attributes), and every header resource its includes read moved to `ORG_HEADER_SRV`
  (44 register-bound declarations in feature headers are not yet), or the world features undefined in the menu build. Its targets: RT0
  (kMAIN for scheme 4, else what UI::Render bound: kMAIN or the framebuffer) and kMOTION_VECTOR, kMAIN's depth with the stencil write
  (mode 2, ref 0xFF) the engine's depth-stencil state describes.
- **Effect.hlsl built for ORG** (enchantment glow, the main menu's mist).
- **Utility's RENDER_DEPTH and RENDER_SHADOWMAP techniques** in the ORG build (the depth pass and the menu lights' shadow maps).
- The TAA mask fill (step 11) and the HDR chain (step 12) are ImageSpace's.
Until then `menu views: menu 3D left to the engine` stays: removing it alone would drop the loading-screen model under the latch.

### World map and race menu (no view needed)

Both are DCLF's main view. To check: the census's report while the menu is open (the main view's residue only: no Lighting member drawn
by the engine), and the shots against the engine path. The world map's clouds and the race menu's face morphs are the risks to look at.

### Boundary with p5-loading

Scheme 4 (MistMenu: the main menu and the loading-screen model) renders on the ServingThread during loading screens. The probe skips
renders off the main thread (counted as such); the loading model is the p5-loading lane's.

## What can trigger a graph rebuild

- The probe: nothing (no client, no import).
- The local map family: two clients registered at setup with every input fixed, joined at the setup's build point; a target made
  again (a resolution change) leaves the batches to the engine until the next start, as the shadow mask family does. Nothing
  registers during play.

## Test plan

1. `menus-org0.env` and `menus-org1.env` (`tools/cs-ab/scenarios/`): Whiterun, then the inventory, magic, stats and journal menus, the
   world map, and `showracemenu` last. The shots must match (the engine draws every menu scene on both).
2. Log (org1): `[DCLF] menu views: probe on`, `hooks installed (menu 3D on, local map on)`, then one `[DCLF] menu views: menu 3D scheme
   ...` line per scheme shape. Read from them: whether the inventory has a DCLF frame (`world frame yes/no`, `freeze-frame menu open`),
   the targets (RT0 1 or 0), the stencil mode (2, ref 0xff), the geometry counts and the verdict. The local map needs its tab opened by
   hand (no test command), then `[DCLF] menu views: local map ...` (orthographic, RT0 17, depth 0).
3. The census (both): with the world map and the race menu open, no Lighting member drawn by the engine.
4. The local map (org1, tab opened by hand): `[DCLF] menu scenes: the first cs.menu-scenes.local-map run drawn by the graph` and the
   same for `local-map-fog`; the map's shot against org0's. A `[DCLF] menu scenes: the engine draws ...` line names each refusal (the
   land's Lighting passes until a Lighting program exists).

## The menu scene's shadow masks

The residue census's Utility draws in the inventory (about 1.7 a frame, `52728+0x1E5` under UI3DSceneManager::Render, RT
`kSHADOW_MASK`, no depth) are not the model's draws: UI3DSceneManager::Render calls the menu scene's shadow masks (SE 51856 at +0x466,
AE 52728 at +0x461), one BSUtilityShader pass over the screen quad per menu light that casts shadows, before step 6's stencil. They are
drawn on the graph by the shadow mask family with `CS_ORG_SHADOW_MASK=1` (p5/shadow-mask.md, its `menu` view), which needs none of
the menu 3D view below. The model's own Lighting draws, step 9, stay the engine's until that view exists.

## Not done

- Menu 3D's family (above): the Lighting, Effect and Utility depth/shadow map programs first, then the batches.
- The local map's land: Lighting's land technique in that program.
- No item is selected in the inventory scenario (no test command selects one), and the local map tab needs opening by hand.
