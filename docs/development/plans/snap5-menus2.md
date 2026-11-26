# snap5-menus2: menu scenes, second round (roadmap phase 5, "Menu scenes")

Lead `menus2`, branch `snap5/menus2` (base `org_integration` 00d95ac1d, org_integration merged again at 8febaa95b for snap3-llspec),
stage `s7-menus2`. Starts from [snap5-menus.md](snap5-menus.md); decisions 146-150.

## State

The item is **not finished**. The stats menu (scheme 6) is switched: CS computes and draws every draw of it, the engine's
UI3DSceneManager::Render is not entered for it, and its shot matches the engine-drawn one. MistMenu (main menu, loading model),
the inventory, lockpicking, book and the local map are still the engine's.

| Item | State |
|---|---|
| 1. Blended order, stats switch | done: the order matches the engine's in every observed batch; scheme 6 owned; shots match |
| 2. Light lists | done for point lights (a light reaches a geometry its radius touches: 0 differences outside the main menu's first frames); the shadow-light rule (count shadow lights only when the scene renders shadow maps) is not done, it only matters for MistMenu |
| 3. MistMenu | not started: particles (the smoke), the loading model's shadow maps and mask, the menu HDR chain and RenderEndOfFrameEffects |
| 4. Inventory 3D | palettes computed (`BoneRows`, observe compares them with the engine's b9/b10, but no skinned item was observed yet); not switched, see below |
| 5. Local map, lockpick, book | not started; no cs-test step opens them |

## What was built

All in `Features/MenuViews/MenuScenesNative.cpp` unless named.

- **The accumulator's blended order.** Read from AE 1.6.1170: a pass with accumulation hint 1 goes to the batch renderer's blended
  list (+0xF0, `FUN_1414f50b0`) in registration (cull) order, or into the group a `BSOrderedNode` opened with StartGroupingAlphas
  (AE 0x1414f50d0: pushed on a stack in visit order, its key the node bound's centre along the camera direction). FinishAccumulating
  merge sorts the list (`FUN_1414f5250` -> `FUN_1414e8fc0`, stable, descending by `centre . cameraWorldRotate column 0`, less the
  radius when the accumulator's `sortByClosestPoint` (+0x51) is set), and the draw (`FUN_1414f5330`) interleaves: the stack's top
  group is drawn whole, in registration order, before the list's head when its key is larger. `BlendedList` ports which passes get
  hint 1 (BSEffectShaderProperty::GetRenderPasses AE 0x1414bd890: not a decal or billboard flag, and property alpha below one, the
  alpha property blending, a particle system or falloff; Lighting: the alpha property blends). `SceneGeometry` builds the draw order
  from these. The stats constellations are under a BSOrderedNode: that is why no sort reproduced them before.
- **Billboards.** The engine turns an NiBillboardNode to the culling camera in its OnVisible (AE 71063), a side effect of the cull
  the native render does not run. `Walk` calls the same rotation (SE 69681, AE 71061) for each visible billboard node; without it the
  constellations' glows and lines stayed edge-on and invisible.
- **Point lights.** `SelectLights` keeps a scene node light for a geometry when the light's radius reaches the geometry's bound
  (`BSLight::geomList` is empty for menu geometry; the inventory's extra light came from taking every active light).
- **Permutation words** (`PermutationOf`): Linear Lighting's RenderToUI (its RenderMenuScene sets it around UI3DSceneManager::Render),
  BaseTextureIsWorking (a water reflection source only, with RenderToUI) and the Effect SourceAlphaBlend bit per alpha property, as
  LL's SetupGeometry hooks make them. Without them the switched scene was dark. Observe compares them (0 differences).
- **Palettes** (`BoneRows`): a skinned draw's VS b9/b10 from `SkinPalettes::Sample` (the engine's palette update, as DCLF's world
  does), appended to the latch; Lighting skinned draws are no longer refused. Skinned Effect draws still are.
- **Render fixes.** `Family::BeginBatch` now runs before the draws lease geometry (it was after, so leases predated the frame's
  resources), kMAIN's depth clear is checked (fatal if not taken), a draw CS cannot compute in an owned scheme is fatal (decision
  149), and a blended Lighting draw that fades (hint 9 or 10) counts as one.
- **Programs still building.** A draw whose ORG program is still building on the program service is left out of that render and
  logged once; the manifest prebuilds it next session. Same rule as the engine draw families (the sky in graph mode).
- **Linear Lighting.** After snap3-llspec, the ports apply SpecularColor and Effect BaseColor themselves; the two local blocks are
  deleted. PropertyColor is still built here from the property (converted as LL's EffectGeometryUpload does).
- **Observe** (`CS_MENU_SCENES_OBSERVE`, kept: schemes are still unswitched): the order check compares the blended list only (hint 1
  against `BlendedList`) and logs each draw's hint, key and ordered-node membership on a difference; palettes, permutation words and
  a per-geometry light dump were added.
- `Owns` holds scheme 6.

## Why the inventory is not switched

Decision 149 makes any draw CS cannot compute fatal in an owned scheme. Item previews can carry draws the menu path does not compute:
enchantment glows (Effect membrane), skinned Effect draws, particles, projected UV. Owning schemes 1 and 2 would end the game on such
an item. The TAA steps the previous notes listed are not needed: CS turns the engine's TAA off around UI::Render (Hooks.cpp,
`MenuManagerDrawInterfaceStart`), so UI3DSceneManager::Render takes its non-TAA branch for every scheme (no motion target, no TAA mask
fill, no TAA UI copy).

## Decided by the agent, to confirm

- **A draw whose program is still building is left out, not fatal**: it is a transient the next session's manifest removes, the
  engine draw families already treat it so, and a fatal would end every first session that opens the stats menu.
- **The billboard turn is the engine's CPU function**, called on the game side in the update half (as decision 147's other CPU
  calls): it is the cull's only side effect the draws need.
- **A light reaches a geometry by bound distance** (light radius plus bound radius), not through the engine's light lists.

## Proposed decision-log entries

- The stats menu (scheme 6) is CS's: draw order from the accumulator's blended list model (hint 1, stable far-to-near merge sort,
  BSOrderedNode groups interleaved from a stack), billboards turned by the engine's CPU rotation, light reach by bound, permutation
  words as Linear Lighting's hooks set them. Its UI3DSceneManager::Render site reads producer.
- CS's UI::Render hook keeps the engine's TAA off, so menu scenes never need the TAA mask fill or the TAA UI form.

## Open items, in order

1. MistMenu: particles (the main menu smoke, `PArray01`, refused as an Effect variant), the shadow-light rule (at the main menu the
   shadow light is a plain light with no shadow bits; in loading frames the first frames assign lights before the shadow render), the
   loading model's shadow maps and mask (`FUN_140973210`, the scene node's shadow lights, iLoadingMenuShadowLightFlags), the menu HDR
   chain from ImageSpaceManager+0xB0 and RenderEndOfFrameEffects (today latched in `MenuSteps.inl`).
2. Inventory: Effect membrane, skinned Effect and particles in the menu path; then own schemes 1 and 2. Verify `BoneRows` against
   the engine's palettes with a skinned item (observe's `palette` fields).
3. Lockpick (7), book (3: its pages are a Scaleform render target the native AddTexture does not import), journal (0): test steps.
4. Local map: its own native path (Utility local-map passes, fog of war, ISLocalMap).
5. Delete `CS_MENU_SCENES_OBSERVE` with the last scheme's switch.

## Runs (native Vulkan unless named, `game.save` from the base config)

| Config | Label | Result |
|---|---|---|
| menu-scenes, observe + detector | s7-menus2-obs1-scenes | passed; blended membership 0 differences; order still differs (no ordered groups yet) |
| menu-scenes, observe | s7-menus2-obs2-scenes | passed; stats order matches; geomList light lists wrong (449 of 451) |
| menu-scenes, scheme 6 owned | s7-menus2-sw1-scenes | hang: fatal "program cannot be built" (a program still building) |
| menu-scenes, scheme 6 owned | s7-menus2-sw2..sw4-scenes | passed; shots too dark, world through the scene, then glows missing: fixed in turn (BeginBatch, permutation words, billboards) |
| menu-scenes, scheme 6 owned, detector | s7-menus2-sw5-scenes | passed; stats shot matches the engine-drawn native shot (`s7-menus2-obs2-scenes`); stats site producer |
| guest+quick, native-vulkan+quick, native-d3d12-quick | s7-menus2-fin-guest-quick, -fin-nvk-quick, -fin-d3d12-quick | passed |
| native-vulkan+menu-scenes (detector), guest+menu-scenes | s7-menus2-fin-nvk-scenes, -fin-guest-scenes | passed; native: stats site producer, 16 engine render calls per world frame (36.7 in the first round's baseline run, which also counted the stats renders) |
| native-vulkan+menu-main (detector) | s7-menus2-fin-nvk-main | passed; 20 engine render calls per main-menu frame (MistMenu unswitched) |
| native-vulkan+cadence-drop-cells | s7-menus2-fin-dropcells | passed |

Detector (`[EngineZones]`): before (s7-menus base run) `UI3DSceneManager::Render (in StatsMenu::PostDisplay) [render]` with its inner
sites (RenderPreResolve/PostResolve batches) once per stats render; after, the stats site is `[producer]` and the inner sites are
entered only by MistMenu and the inventory/journal renders. Main-menu and loading frames are unchanged (MistMenu is not switched):
22 engine render calls per main-menu frame.
