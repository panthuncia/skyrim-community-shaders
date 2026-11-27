# snap5-map: the world map and the local map in native mode (roadmap phase 5, "Menu scenes")

Lead `map`, branch `snap5/map` (base `org_integration` a9182dd5f), stage `s7-map`. Starts from [snap5-book.md](snap5-book.md),
[snap5-menus5.md](snap5-menus5.md), [snap5-menus4.md](snap5-menus4.md) and decisions 146-150, 167-169, 183-191, 201-203.

## State

| Item | State |
|---|---|
| 1. World map crash (decision 203) | done: root cause found; the world map opens, renders and closes in native Vulkan and D3D12; guest unchanged |
| 2a. World map view | done: CS's. Map frames read 1.0 engine render calls (only Main::DrawFrame) |
| 2b. Local map | works in native and guest (it hung native before), but still drawn by the engine draw family (latched), not computed by CS. 35 engine render calls per local map frame. Port open (below) |
| 3. Delete `CS_MENU_SCENES_OBSERVE` | done: every light scheme with a 3D scene is owned; no menu pushes scheme 5 |

## 1. The world map crash: root cause

`s7-book-base-nvk-map` crashed at SkyrimSE+0xE4B9BB (`mov rbx, [rax+0x48]`) in SetDirtyStates, called from
`ImageSpaceEffect::Render` (AE 107446) under the engine's `RenderEndOfFrameEffects`.

- A world map frame turns end-of-frame slot 0 (ImageSpaceEffectWorldMap) on. `EofNativeFrame` kept every frame with slot 0 on the
  engine's `RenderEndOfFrameEffects` (decision 7 of native-post-chain.md), in native too.
- The world map effect's Render is the base `ImageSpaceEffect::Render`: per pass it calls the shader's Render (CS's detour stages it
  natively: "first ISWorldMap staged natively"), then `SetDirtyStates(false)` itself.
- That call applies the engine's pending state. `DIRTY_VERTEX_DESC` was set and no engine vertex shader had ever been bound, so it
  read the null shader's vertex description: the same crash as decision 188, at another engine site.
- It passed at 17:32 (s7-quarter) because something engine-drawn had bound a vertex shader earlier. The loading model was the last
  engine render that did; menus5 (decision 189) made it CS's, and the world map then crashed every time.

Fix: native stages slot 0 itself (`WorldMapSteps`, NativePost.inl), so the engine's container is never entered. Rows from
ImageSpaceEffectWorldMap::UpdateParams (AE 107802 at 0x1414fdf40, SE 0x141311ca0):

- id 0: (-1e8 (0x141abf108, the DoF's "no range"), near, far - near, far * near) of the world camera (414580 / 527667 +0x160/+0x164;
  the snapshot's main camera, as the other slots).
- id 1: four floats MapMenu writes (AE 415301 / SE 528355).
- id 2: (0.5 / width, 0.5 / height, 0, 0) of the destination target (0.5 at 0x141ad285c).
- The shader is the effect's list entry 0 (Setup, 0x1414fdb70, picks ISWorldMap or ISWorldMapNoSkyBlur at creation). t1 is kMAIN's
  depth, which the chain step already declares.

Guest keeps the world map on the engine path (the frame's end-of-frame effects as before). Decided by the agent, to confirm: guest is
out of scope, and its engine path draws.

## 2. The local map

### How it renders (RE, AE 1.6.1170)

- `MapMenu::PreDisplay` (vtable slot 7, AE 53103 / SE 52216) is `add rcx, 0x60; jmp LocalMapMenu::PreDisplay` (0x14097bf20), which
  calls `LocalMapRenderer::Render` (AE 16335 / SE 16094) while the local map shows (+0x303fc).
- `LocalMapRenderer::Render`: BSShaderManager state +0x219 set (local map mode), tree LOD mode 1, culls with its BSCullingProcess
  (+0x301f8) the loaded cells (FUN_140243b00 / FUN_140243ea0, or the interior or sky cell), Main's +0x58 root and
  `BSShaderManager::State` roots 9 and 8; `SetCurrentDepthStencilTarget(kMAIN, clear)`, `SetRenderTarget(0, kLOCAL_MAP_SWAP, clear)`,
  SubmitAccumulator; with fog of war on: render mode 0x13, culls the fog root (+0x30358), depth mode 8, SubmitAccumulator into
  kLOCAL_MAP_SWAP (restore); then `RenderEffect(ISLocalMap = 0x62, kLOCAL_MAP_SWAP, kLOCAL_MAP, param)` with row 0 = 1 / size.
- MapMenu::ProcessMessage (AE 0x140984a80): user event `LocalMap` (UserEvents +0x328) toggles the local map.

### What was built

- **MapMenu::PreDisplay runs at DrawInterface's start** in graph mode (native and guest), as BookMenu's does: `OffscreenPreDisplay`
  (Renderer.cpp) now serves both menus (the map's through its vtable slot, since its PreDisplay is a jump stub).
- **ISLocalMap is a menu composite step** (`MenuSteps.inl`, part 0, `InOut(kLOCAL_MAP_SWAP, kLOCAL_MAP)`), after the local map's batches
  and before the displays.
- **The skeleton clears kLOCAL_MAP_SWAP** at the UI stage's places (`kColourTargets`, `PlaceClears`). It stayed on D3D11 before,
  which native does not have.
- **Scaleform displays read engine targets** (`Passes.cpp`): the map movie samples kLOCAL_MAP. `DeclaredRead` declares an engine
  target found by its texture (GFx holds its own view of it); the recording puts the declared read's index in. A display whose engine
  target read is new waits one revision (lag, not a refusal).
- **The local map's batch clients repeat** (`MakeDesc`): its land batch and its fog batch tap their shared UI step one after the other.
- **The local map batch latches up to 4 MiB** (`LatchBytesOf`): a Whiterun local map overran 1 MiB, and the rest of the batch went to
  the engine (dropped in native).
- **Test harness:** `uievent <menu>=<user event>` command; config `map-local` (Whiterun at noon, world map, local map shown, two
  shots, local map hidden, world map shot, close). Pass it by path.

Before this, the local map never ran in a CS stage: in native it hung (ISLocalMap had no pass, then the displays could not read
kLOCAL_MAP); in guest ISLocalMap drew nothing (a tap error, so kLOCAL_MAP stayed empty).

### Shots

Native and guest local map shots match each other (`ref check` pass), and the world map behind matches exactly. Both differ from
plain D3D11 (`s7-map-a7-d3d11-local`): the panel is opaque black where D3D11 shows the world map through it (kLOCAL_MAP's alpha), and
a few land tiles draw white. That is the graph path of the engine draw family in both modes, not native-specific. Open item 2.

## 3. `CS_MENU_SCENES_OBSERVE`

Light schemes pushed by `UI3DSceneManager`'s scheme push (AE 0x1409738f0), from its callers: 0 (the journal, two callers, no 3D
scene), 1 and 2 (Inventory3DManager::Begin3D from barter, container, crafting, gift, inventory, magic), 3 (book, two), 4 (loading
menu, MistMenu), 6 (stats), 7 (lockpicking). No caller pushes 5 (AE checked; SE not). Every scheme with a 3D scene (1, 2, 3, 4, 6, 7)
is owned, so the switch, `MenuScenesObserve.cpp`, the capture's observe calls, the menu HDR observe (NativePost.inl, MenuSteps.inl,
ImageSpace.cpp) and the registry entries are deleted. The latched capture stays: guest's menu scenes and the local map use it.

## Detector (`[EngineZones]` exit lines; Dev-Fast)

| Frames | Before | After |
|---|---|---|
| world map (counted as world frames) | fatal at the first map frame (`s7-book-base-nvk-map`) | 1.0 per frame, Main::DrawFrame only (`s7-map-a1-nvk-map`: world 1.0, max 1) |
| local map (frozen world frames) | hang at the first local map frame (`s7-map-a2-nvk-local`) | 35.0 per frame, max 43 (`s7-map-a9-nvk-local`): Main::DrawFrame, the two batches' RenderBatches/RenderGeometryGroup sites, RenderEffect in LocalMapRenderer::Render and in AE 107130 |
| world | 1.0 | 1.0 |

Native post chain log in the map run: end-of-frame frames on the engine 0, slot effects through RenderEffect 0.

## Decided by the agent, to confirm

- **Guest keeps the world map's end-of-frame on the engine path**; native stages it. Guest is out of scope and draws today.
- **MapMenu::PreDisplay moves to DrawInterface's start in guest too** (as the book's): guest's ISLocalMap had no pass outside the UI
  stage, so the local map was empty in guest.
- **The local map stays the engine draw family's (latched) for now**: a full port (below) is a larger item than this round.
- **`CS_MENU_SCENES_OBSERVE` is deleted** although the local map is not ported: it compares menu scheme draws only; the local map's
  port needs its own observe step.

## Proposed decision-log entries

- Closes 203's world map crash: a world map frame's end-of-frame ran on the engine (decision 7), and ImageSpaceEffect::Render's own
  SetDirtyStates read a null vertex shader once no engine render had bound one (the loading model was the last, until 189). Native
  stages the world map slot (ImageSpaceEffectWorldMap::UpdateParams' rows); guest keeps the engine path. World map frames: fatal ->
  1.0 engine render calls.
- The local map runs in the UI stage in native and guest: MapMenu::PreDisplay at DrawInterface's start, ISLocalMap a menu composite
  step, kLOCAL_MAP_SWAP cleared at the UI places, Scaleform displays read engine targets through declared reads. Its draws are still
  the engine draw family's (35 engine render calls per local map frame).
- `CS_MENU_SCENES_OBSERVE` is deleted: every scheme with a 3D scene is CS's; nothing pushes scheme 5; the journal (0) has no scene.

## Open items

1. **The local map's port** (decision 149: observe, then switch). Needed: CS's visibility for its ortho camera over the cells, LOD
   roots and fog root (no engine culls); the passes the accumulator gets in render mode 0x12 (Lighting properties become Utility
   RENDER_BASE_TEXTURE passes, SE 0x1412c7cd0; landscape keeps Lighting; water takes its local map technique, unported in the water
   port's `localMap` path; tree LOD has no ORG program); the fog of war (render mode 0x13, Utility LocalMapFogOfWar, depth mode 8);
   their states and order; ISLocalMap's row from the target size. An observe step against the latched batches first.
2. **The local map's look in graph mode** (native and guest alike): kLOCAL_MAP's alpha is opaque where D3D11 leaves it clear, and some
   land tiles draw white. Check the latched land Lighting draws in local map mode and the clear colour's alpha.
3. Each local map run first refuses the Utility programs 0x10000003 (fog) and 0x200001a (objects), which are not prebuilt: the rest of
   that batch is dropped in native for a frame (the manifest lists them for the next session).
4. SE: scheme 5's callers were checked on AE only.

## Runs (Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-vulkan+map (detector) | s7-map-a1-nvk-map | passed; map shots match the engine-path native run `s7-quarter-c` (ref check pass) |
| native-vulkan+map-local | s7-map-a2-nvk-local .. -a5 | hang: displays could not read kLOCAL_MAP, then two taps of the local map's step (fixed) |
| native-vulkan+map-local | s7-map-a6-nvk-local, -a7 | passed; a6 lost part of the batch to the 1 MiB latch (fixed in a7) |
| guest+map-local | s7-map-a7-guest-local | passed; ref check of native a7 against it: pass |
| d3d11+map-local | s7-map-a7-d3d11-local | passed; plain D3D11 reference (open item 2) |
| native-vulkan+map-local | s7-map-a8-nvk-local | fatal: kLOCAL_MAP_SWAP's clear had no UI place (fixed) |
| native-vulkan+map-local (detector) | s7-map-a9-nvk-local | passed |

Final runs (`s7-map-fin-*`, on 0b0632ba6's code), all passed: native-vulkan+map (detector; `-fin-nvk-map2`, the first try timed out
at the main menu before any map step: the main thread waited in the UI's menu lock, no CS frame on the stack, not seen again),
guest+map, native-d3d12+map, native-vulkan and guest +map-local, native-vulkan+quick, guest+quick, native-vulkan and guest
+menu-scenes, native-vulkan and guest +menu-book (native 4 engine render calls per world frame: the page copies), native-vulkan
+menu-main, +menu-loading, +menu-items.

Skipped under the director's test scope rule: native-d3d12-quick (no ORG, BasicRHI or native device change; native-d3d12+map covers
the world map in D3D12) and native-vulkan+cadence-drop-cells (nothing stepped by game delta changed).

Shot references to accept: `s7-map-fin-nvk-map2` and `s7-map-fin-guest-map` (map, map-b; checked against the engine-path native
`s7-quarter-c`), `s7-map-fin-d3d12-map`. The local map shots are not references yet (open item 2).
