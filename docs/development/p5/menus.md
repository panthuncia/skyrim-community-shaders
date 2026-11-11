# P5.8 and P5.9: the menu scenes as DCLF views

Plan tasks P5.8 (menu 3D) and P5.9 (map menu) ([org-full-port.md](../org-full-port.md), Phase 5). Reverse engineered with GhidraMCP on
SE 1.5.97 and AE 1.6.1170 (2026-11-10). IDs are Address Library IDs. The code is `src/Features/DrawcallLimitFix/Features/MenuViews/` and
the switches are `CS_ORG_MENU3D=1` and `CS_ORG_MAP=1` (default off).

**State:** the RE is done, the design is below, and the first stage, the probe, is written. The probe reads what each menu scene's view
needs and logs it; it withholds nothing and registers no graph client. The views themselves are not written yet (see "Not done").

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

1. `BSShaderManager::State+0x219 = 1` (the local map mode: Lighting's `GetRenderPasses` registers BSUtilityShader local-map passes,
   `LOCALMAP_FOGOFWAR` in CS's Utility.hlsl), the accumulator's shader state (+0x148), `BSTreeManager` LOD on (`FUN_1403de280`),
   clear colour 0.
2. Culls (`BSCullingProcess::Process` on +0x301F8) the cells' nodes, then the LOD roots (BSShaderManager::State+0x118's +0x48 and +0x40)
   with the local map camera (+0x302B8, orthographic).
3. Depth kMAIN CLEAR, `RT0 = kLOCAL_MAP_SWAP (0x11)` CLEAR, **`SubmitAccumulator(cam, acc (+0x302C8), 0)`** (SE +0x340, AE +0x342).
4. If `DAT_141db3ec8`: render mode 0x13, the water roots culled, depth mode 0, `RT0 = 0x11` RESTORE, **`SubmitAccumulator` again**
   (SE +0x430, AE +0x433) with depth mode 8 (water), then depth mode 3.
5. Unbinds depth, sets the fog-of-war parameters (+0x302D0, `FUN_1412d66f0`), **ImageSpace effect 0x62 (`BSImagespaceShaderLocalMap`)
   from 0x11 into kLOCAL_MAP (0x10)**, restores the state.

No stencil. The members are world objects DCLF already tracks (the loaded cells' objects, LOD land, objects and trees), drawn with
another camera and another program family (the Utility shader's local map technique).

## Design, scene by scene

Rules the design keeps: a view is registered at install with a fixed shape, so opening a menu never registers a client or changes a
shape (no graph rebuild); a scene is withheld only when its view claims every geometry of it; anything else the engine draws.

### The probe (written, stage 1)

At each menu scene's SubmitAccumulator (main thread only) `MenuViewsProbe.cpp` reads the targets, depth and stencil mode the engine set,
the camera's projection, whether DCLF's frame ran since the scene's last render (its frame number moved) and whether a freeze-frame menu
is open, and for menu 3D the scheme's root geometry: how many are Lighting, skinned, tracked by DCLF's installed tables and claimed by a
set phase. A change logs one line with a verdict (the first input a view lacks). It never withholds and registers no graph client.

Expected first results: menu 3D's geometry is not tracked (no view names the menu roots as category roots yet), and the inventory may
have no DCLF frame (if it freezes the world).

### The menu 3D view (stage 2, not written)

A third bucketed view on Core's view contract (`Api/Views.h`), like first person:
- `Api::EngineView::Menu3D`, its `kViewSlots` row (key group, extension id `cs.dclf.menu3d`), its epoch `menu3d` (trigger
  `menu3d.render`, the Submit thunk; passes `view.build-draws`, `view.colour`, `view.forward`, `view.direct`) and its
  `RenderGraphRuntime::Segment`. The set phase is the last free bit (`1u << 7`), the view bit `1u << 12`, a forward key bit of its own.
  Core's places that branch on the two views today (Direct's `ViewType`, `ViewSites.cpp`, `ViewEngine.cpp`'s type choice,
  `ViewBatchesInternal.h`, `Cull.cpp`'s main family) need the third case.
- Membership `CategoryRoot`: the root is the current scheme's `menuObjects[scheme]` (parentless, as the first-person root is). A scheme
  change is a category signature change: the scene lane re-files at its next pass and the next publication carries it, with no graph
  rebuild. Better: capture all eight roots once (a root list in `ViewRootCapture`), so a menu change is only a hidden-bit change.
- Withholding `InMainRenderers` with `accumulators` = mgr+0x10 and +0x18. Covered only when the probe's verdict is clean for the
  current root: every geometry a member, the epoch's client in the built graph (`FrameAnchors::InBuiltGraph`), and a DCLF frame this frame.
- Targets: kMAIN's depth imported once (`DepthTarget`), cleared by the pass (depth 1, stencil 0) since the engine's clear runs at the
  withheld draws' `ApplyState`; RT0 both kMAIN and the framebuffer imported at install with two colour passes in the shape, one chosen per
  frame by a latch (both recorded; no rebuild); RT1 kMOTION_VECTOR. Its pipelines write stencil bit 0 (ALWAYS REPLACE, ref 0xFF, write
  mask 1: the view's key bit selects it), so the engine's TAA mask fill (step 11, stays the engine's) reads the same stencil.
- Lighting: the view's own frame constants from the menu's `SetPerFrameBuffers` (as the reflection's `LatchFrameBuffer`), the menu
  scheme's directional and ambient light in place of the main rows', no Light Limit Fix clusters (null t35-t37) and the scheme's point
  lights as the view's own list. This is the largest new piece (a shading contribution per view).
- **Freeze-frame menus:** without a world frame DCLF adopts nothing. The view needs a menu frame: at UI::Render, adopt the newest
  publication and run `host->BuildIfRequested()` between frames, as Bink does. Until then the view covers only menus with a world frame.

### The local map view (stage 3, not written)

A view of the one scene like the reflection's faces: members are the main set's objects and LOD (`LodTechniques`-like membership over the
main set, the cells the local map culls), `OwnRenderers` = the accumulator at +0x302C8 while `LocalMapRenderer::Render` runs. One face,
orthographic, its own depth, into kLOCAL_MAP_SWAP (0x11) imported once. The program family is new: the Utility shader's local map
technique (`LOCALMAP_FOGOFWAR`), not the Lighting colour pipelines. The water pass (mode 0x13) stays the engine's (the p5-water lane).
The local map renders only when the tab opens or pans: the epoch runs on those frames, its shape fixed, so nothing rebuilds.

**Boundary with p5-imagespace:** effect 0x62 (`BSImagespaceShaderLocalMap`, 0x11 -> 0x10) is theirs. The view writes 0x11 through the
engine target's graph resource (`FrameAnchors::EngineTarget(kLOCAL_MAP_SWAP)`), the same resource their pass reads, so the graph orders
it; nothing here touches 0x10.

### World map and race menu (no view needed)

Both are DCLF's main view. To check: the census's report while the menu is open (the main view's residue only: no Lighting member drawn
by the engine), and the shots against the engine path. The world map's clouds and the race menu's face morphs are the risks to look at.

### Boundary with p5-loading

Scheme 4 (MistMenu: the main menu and the loading-screen model) renders on the ServingThread during loading screens. The probe skips
renders off the main thread (counted as such); the loading model is the p5-loading lane's.

## What can trigger a graph rebuild

- The probe: nothing (no client, no import).
- The planned views: registered at install with fixed shapes; their targets imported once; a scheme or menu change only changes the
  scene lane's filing (a publication, not a graph build); RT0's two cases are two passes chosen by a latch. A target resize (a resolution
  change) re-imports, as every engine target does.

## Test plan

1. `menus-org0.env` and `menus-org1.env` (`tools/cs-ab/scenarios/`): Whiterun, then the inventory, magic, stats and journal menus, the
   world map, and `showracemenu` last. The shots must match (the engine draws every menu scene on both).
2. Log (org1): `[DCLF] menu views: probe on`, `hooks installed (menu 3D on, local map on)`, then one `[DCLF] menu views: menu 3D scheme
   ...` line per scheme shape. Read from them: whether the inventory has a DCLF frame (`world frame yes/no`, `freeze-frame menu open`),
   the targets (RT0 1 or 0), the stencil mode (2, ref 0xff), the geometry counts and the verdict. The local map needs its tab opened by
   hand (no test command), then `[DCLF] menu views: local map ...` (orthographic, RT0 17, depth 0).
3. The census (both): with the world map and the race menu open, no Lighting member drawn by the engine.

## Not done

- The menu 3D and local map views (stage 2 and 3 above): a third and fourth engine view through Core's view machinery, the menu
  lighting, the stencil key, the menu frame for freeze-frame menus. Not started because each changes Core's two-view code paths in many
  places that cannot be checked without a build; the probe's first run decides the order (freeze frames, tracking).
- No item is selected in the inventory scenario (no test command selects one): the inventory's scheme may log an empty root.
