# snap5-menus: menu scenes (roadmap phase 5, "Menu scenes")

Lead `menus`, branch `snap5/menus` (base `org_integration` 175c008da), stage `s7-menus`. Roadmap:
[render-thread-snapshots.md](render-thread-snapshots.md), phase 5. Decisions 15-17, 63-65, 71+ and 117. Older RE:
[engine/menus.md](../engine/menus.md).

## State

The item is **not finished**. What exists is the base the next lead switches scenes on: CS computes every constant of a menu
scene's Lighting and static Effect draws from the scene graph, with observe parity against the engine's latched draws, and the
native render path (update half game-side, draw half CS's) is written and was run once on the stats menu. No scheme is switched in
this branch's final build, because the stats menu's blended draw order does not yet match the accumulator's (below).

| Item | State |
|---|---|
| Lighting draws' constants from DCLF's ports | done: 0 differences in every row the engine writes, but the previous-frame rows (decided below) |
| Static Effect draws' constants | done: 0 differences, the same exception |
| Draw states (blend, depth, cull, write mode, alpha test, viewport) | rule written from the observed engine states (`StateOf`) |
| The visible set (hidden, switch nodes, the menu camera's frustum) | matches the engine's set in the stats menu |
| Blended draw order | not matched: neither far-to-near by distance nor by view depth reproduces the accumulator's order |
| Native render path (`Native::Render`) | written; ran on scheme 6 (`s7-menus-sw1-scenes`): draws, detector reads producer, picture differs (order) |
| MistMenu (main menu, loading model) | blocked: the main menu's smoke is a particle system; the loading model has shadow lights (iLoadingMenuShadowLightFlags) |
| Inventory 3D (scheme 1) | needs the TAA mask fill and the TAA UI form as native post steps, and skinned palettes for some items |
| Lockpick, book, journal | not reached in a run (no test command opens them); the journal opened no 3D scene |
| Local map | not started |
| Menu end-of-frame chains (HDR, copy, TAA UI) | not started |
| Loading frames' reflection cube faces | never seen: in every run the loading frames' pre/post-resolve batches are the loading model's (370 of 370) |

## What was built

### `Features/MenuViews/MenuScenesNative.cpp` (`Scenes::Native`)

- `SampleInputs`: one menu render's inputs: the menu camera (`FrameCamera::Capture`), its previous camera, technique inputs,
  the menu scene node's pipeline frame (`GeometryPort::SamplePipelineFrame(sun, eyeZero)`, a small additive overload), the material
  frame. It sets `EngineShaders::Lighting()` when DCLF has not (main menu: no world frame yet), or the material port refuses all.
- `SelectLights`: the pass's lights as GetRenderPasses gives a menu pass: the menu scene node's sun, its active shadow lights, then
  its active point lights.
- `ComputeLighting`: descriptors (`DeriveLightingDescriptors` plus the shadow lights' bits), PerTechnique (`EvaluateTechnique`),
  PerMaterial (`MaterialPort`), PerGeometry (`PipelineGeometryConstants` plus World/PreviousWorld, `SampleShading`, and the
  point light rows as the latch writes Light Limit Fix's strict lights), packed through the bound shader's constant tables, with a
  mask of the floats the engine writes. Linear Lighting's upload hook (SpecularColor, `LightingMaterialUpload`) is applied.
- `ComputeEffect`: the same for BSEffectShader: fog and FilteringParam (AE 391710/391713, SE 513745/513747), the Effect material
  port, Linear Lighting's BaseColor and PropertyColor hooks, World/PreviousWorld, PropertyColor. Particles, membrane, lit, soft,
  blood, skinned and indexed-texcoord variants are refused.
- `SceneGeometry`: the visible geometry (hidden subtrees, switch nodes, the menu camera's frustum through `ShadowCameras`).
- `Render`: UI3DSceneManager::Render (AE 52727) for an owned scheme: lock and scheme test, the update half as the engine runs it
  (menu light chain AE 52749 / SE 51876, light into the scene node AE 106341 / SE 99707, `NiAVObject::Update` on the root, the
  scene node light update AE 106335 / SE 99701), then the colour pass built from the scene graph into CS's frame (viewport from
  `Family::FrameViewport`, depth range 0..1), kMAIN's depth clear through `Skeleton::ClearDepth`, submitted to the existing
  `cs.menu-scenes.menu-colour` client. `Owns(scheme)` holds the switched schemes; it is empty.
- The detour in `MenuViewsFeature.cpp` calls `Native::Render` for an owned scheme instead of the engine's function.

### Engine-render detector (`EngineZones.cpp`)

`Role::MenuScene`: the three UI3DSceneManager::Render call sites (Lockpicking, MistMenu, Stats PostDisplay) count as render calls
until CS owns their scheme, then as producers (decision 16). Per-site flag refreshed at each Begin. Run `s7-menus-sw1-scenes` with
scheme 6 owned: `UI3DSceneManager::Render (in StatsMenu::PostDisplay) [producer]`, and none of the function's inner sites entered.

### Observe (`MenuScenesObserve.cpp`, switch `CS_MENU_SCENES_OBSERVE=1`, native only)

At each latched menu draw it computes CS's draw and compares: pass and shader descriptors, every written float of the six
constant groups, textures and sampler modes, the frame rows, the point lights, the engine's draw states, and per batch the draw
order. Kept (behind its switch) for the next lead; delete it with the last scheme's switch.

### Configs

`menu-main.toml` (the autoload mod off: the main menu's model, two shots), `menu-scenes.toml` (the loading model under the load
hold, an inventory item, the journal, the stats menu). Pass by path from the main checkout.

## Observe results (`s7-menus-obs4..10-scenes`, native Vulkan, `game.save` Save91)

- Lighting, ~1,200-4,400 draws a run (main menu logo, loading models, inventory items): descriptors 0 differences, PerTechnique,
  PerMaterial, PerGeometry 0 in every written float, textures and address modes 0. Remaining, explained:
  - the shadow light's bits and rows on the first frames: the engine assigns lights before the loading model's shadow render adds
    the shadow light; and at the main menu (no shadow maps) the shadow light is a plain light with no shadow bits. `Render` must
    sample the light lists right after the light update and count shadow lights only when the scene renders shadow maps;
  - PreviousWorld and FrameBuffer c16/c41 (below);
  - FrameBuffer c20-c23 differ by one ulp: `FrameCamera::WriteCamera` inverts in double, the engine in float (as the world camera).
- Effect (stats, main menu): all rows 0 differences but PreviousWorld; particles (PArray01, the main menu smoke) refused.
- Engine states seen: Lighting blend 0, depth 3, write 1 (kMAIN) or 12 (back buffer); Effect blend 1 (dest InvSrcAlpha) or 2 (dest
  One), depth 1 (3 with z-write), write 11; cull 0 for two-sided; stencil mode 2 ref 0xff in every colour draw; viewport the full
  target; depth range 0..0.999998 when a world frame ran before (a stale engine global), 0..1 otherwise.
- Draw order (stats): the visible set matches once culled by the menu camera's frustum; the order of the blended draws does not
  (engine: `StarsBackground Panarama StarsNear09 Panarama02 StarsNear01 Panarama04 WizHaloStreaks`; distance or view-depth sorts
  give other orders). Next step: read the accumulator's blended pass group sort (BSBatchRenderer, the effect hint groups).

## Decided by the agent, to confirm

- **The menu scene's previous-frame data is its own last render** (PreviousWorld, c16, c41), first render = current. The engine
  takes whatever camera SetCameraData had last (the world's after a world frame, so a menu model's motion vectors are the world
  camera's offset); CS gives the model correct motion. Excluded from parity.
- **Menu draws use the depth range 0..1**, not the stale world value.
- **The update half keeps calling the engine's CPU functions** (light chain, scene node light add and update, root update), as
  decision 8 allows. The texture-streaming mark (AE 106451/106452) around the engine's draw is not called: CS's draws hold their
  own texture references.
- **Write mode**: 1 into kMAIN, 12 into the back buffer (the engine inherits it from the frame; seen both for one loading model).
- **A draw CS cannot compute is left out and logged once**, as native already drops refused latched draws; a scheme is switched
  only when none is left out in practice.

## Found on the way

- DCLF's world material port does not apply Linear Lighting's specular colour conversion (`LightingMaterialUpload`, PS 25 in
  working space): the engine path's menu draws have it, CS's computed rows did not until added here. Check DCLF's world rows.

## Open items, in order

1. The accumulator's blended order (stats), then switch scheme 6; detector producer already wired.
2. Light lists sampled after the light update; shadow count only with shadow maps.
3. Inventory: native TAA mask fill (ISSimpleColor, stencil 0x1c) and TAA UI form rows (AE 1414d88b0 mode 2), skinned palettes.
4. MistMenu: particles (port the engine's particle quad stream, or DCLF's `ParticleSnapshots::Expand`), the loading model's shadow
   maps and shadow mask, the menu HDR chain (ImageSpace +0xB0 override data captured game-side, `HdrSteps` keyed into `MenuSteps`).
5. Local map, lockpick and book configs (test commands to open them).

## Runs

| Config | Label | Result |
|---|---|---|
| native-vulkan+menu-scenes (detector) | s7-menus-base-nvk-scenes2 | passed; baseline: UI3DSceneManager::Render from MistMenu main_menu 58 / loading 381 frames, its inner sites (pre/post-resolve batches, RenderEndOfFrameEffects, RenderEffect #2) once per menu frame; stats 188 world + 84 frozen |
| native-vulkan+menu-main | s7-menus-obs1-main, -obs2-main | passed (observe) |
| native-vulkan+menu-scenes | s7-menus-obs2..10-scenes | passed (observe iterations) |
| native-vulkan+menu-scenes (detector, scheme 6 native) | s7-menus-sw1-scenes | passed; stats site producer; picture differs (order) |
| guest+quick, native-vulkan+quick, native-d3d12-quick (final build, no scheme owned) | s7-menus-fin-guest-quick, -fin-nvk-quick, -fin-d3d12-quick | passed |
| native-vulkan+menu-scenes, guest+menu-scenes | s7-menus-fin-nvk-scenes, -fin-guest-scenes | passed |
| native-vulkan+menu-main | s7-menus-fin-nvk-main | passed |
| native-vulkan+cadence-drop-cells | s7-menus-fin-dropcells | passed |

Detector: unchanged by the final build (no scheme owned): main-menu frames still enter UI3DSceneManager::Render and its inner
sites once per frame (about 19 of the ~22 render calls per main-menu frame are this item's), loading frames the same per loading
model render. With scheme 6 owned (`s7-menus-sw1-scenes`) the stats site read producer and its inner sites were not entered.
