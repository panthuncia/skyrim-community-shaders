# snap5-blood: fatal native menu gaps (blood Effect, every loading model, skinned partitions)

Lead `blood`, branch `snap5/blood` (base `org_integration` a9182dd5f), stage `s7-blood`. Starts from decisions 149, 169, 184, 187-190
and 202-203, [snap5-book.md](snap5-book.md) and [snap5-menus5.md](snap5-menus5.md).

## State

| Item | State |
|---|---|
| 1a. The blood Effect variant (EdgeBlood03, pass 0x4842) | done: computed; observe 0 differences |
| 1b. Every loading-screen model computed by CS | done: all 266 distinct models shown, owned native run passes; two more unported variants found and ported (Soft, lit Effect) |
| 2. Skinned menu shapes with several partitions | done: one draw per drawn partition |

## What was built

All in `Features/MenuViews/MenuScenesNative.cpp` unless named.

- **Blood** (`ComputeEffect`): SetupGeometry's Blood branch (AE 0x1414eab10) writes AlphaTestRef.xyz: the alpha property's threshold
  times the exe's 1/255, then BSEffectShader's two blood globals (`Effects::Address::kEffectBlood`, AE 0x1433dccf0/f4). The material
  port already skips the working-space conversion for Blood. `EF::Blood` leaves the unported mask.
- **Soft Effects** (found by the sweep: `p_Dust` on `DLC2BlackBook01`, `TreePineForestBroken04_Smoking` on `LoadScreenBurntWoods01`):
  SetupTechnique's Soft branch (AE 0x1414ea2c0): CameraDataEffect.yz = the menu camera's near and far - near, VPOSOffset = the
  inverse render size (SE 524972/524973, AE 411453/411454). SetupMaterial's LightingInfluence.y = far * near / softFalloffDepth (the
  material port keeps the depth itself for DCLF's shader, so the menu overrides it). **t3 is the null view.** The engine binds
  `Engine::Depth::kMAIN` there (the UI's depth-stencil index 0) while kMAIN's depth is bound for writing, and D3D11 (and DXVK) unbinds
  such an SRV, so the engine's draw reads zero. The capture path (`MenuScenesCapture.cpp`) applies the same rule, so observe can
  compare these draws instead of failing at "a published texture its client does not read".
- **Lit Effects** (`EffectLightRows`): SetupGeometry's light rows for an Effect pass with the Lighting bit: DLightColor from light 0
  (the HDR multiplier times its fade times its colour at +0x14c, or Linear Lighting's conversion), then four point light slots:
  position in model space (the inverse world of AE 0x140e19a20 and D3DXVec3TransformCoord's order) or less the eye for a skinned
  pass, 1 / (radius / world scale)^2, and LOD dimmer times fade times diffuse (Linear Lighting: LightColorToWorking). The lights are
  `SelectLights`' (sun, shadow lights, point lights). `EF::Lighting` and `EF::Soft` leave the unported mask.
- **Particle PreviousWorld**: a particle system's PreviousWorld is its World (SetupGeometry copies the rows); observe showed it.
- **Several skin partitions** (`DrawnPartitions`, `AddGeometry`'s partition argument, `PushDraw`): a skinned shape without renderer
  data draws every partition row 3 draws (DCLF's world rule), each its own draw with that partition's buffers, the same program, frame
  rows and palettes (the bone setter, AE 0x14150be30, uploads the whole skin's palette whichever partition draws). All partitions' buffers
  are taken before any draw is pushed. Lighting, Effect, membrane and Utility (shadow map, depth pass) draws all go through `PushDraw`.
- **Observe**: each distinct "computed" failure is logged past the per-field cap (a draw CS could not compute is fatal when owned). The
  membrane state check took Lighting passes whose pass number has bit 15 for membranes (Lighting pass numbers are above the Effect
  base too); it now checks the pass's shader. The 29 "membrane state" differences of the first sweep were that.
- **Test harness** (`Testing/Commands.cpp`): `loadscreens <frames>[,<substring>]` shows every distinct load screen model (266, by path
  order) in the main menu's Mist Menu, `<frames>` each, through the Mist Menu's own setup (SE 51454, AE 52313) with each screen's object
  data, as LoadingMenu's pick (AE 0x140935870) calls it; it raises signal 900 when done. Config `menu-loadscreens` (main menu, 40 frames a
  model, the shot after the last). `loadscreen`'s per-screen map is shared (`OwnLoadScreens`).

## Observe (native Vulkan, the main-menu sweep `s7-blood-obs2-ls`)

Every draw CS was asked about computed (Lighting 31282, Effect 19286, shadow map 33734: 0 not computed). Blood: EdgeBlood03/10 and
EdgeBlood01 draws: every row 0 differences. Soft (`p_Dust`): 0 differences. Left, explained:
- **The engine's cached pass lists.** GetRenderPasses (AE 0x1414adfb0) rebuilds a property's pass list only when its inputs change. A
  model attached while the colour accumulator's +0x178 is clear (the main menu, keep depth off) keeps passes without the shadow
  lights' bits and light lists without the model's shadow light, also into the first loading-screen frames (`s7-blood-obs3-set`: the
  first load from the main menu shows the same on BurntWoods). CS computes the lists each render. This gives "point lights" (the logo
  and the mist drawn with a loading model's shadow light), "pass descriptor"/"shader descriptors" (DoAlphaTest: the engine's alpha-test
  bit also needs early-Z, `0x14328cc79`, which only the loading screen thread and scheme 1 set), and the lit Effect's light rows
  (`TreePineForestBroken04_Smoking`: one extra light slot). Not fixed: CS's lists are what the engine draws once its lists rebuild.
- World rows of skinned draws (stale, as menus5), the frame rows' ulp (148), shadow map write mode (189), the depth range (148), and
  the PreviousWorld rows of the first render after a model switch (previous eye).
- The sweep runs at the main menu, so keep depth is off: the depth pass and the shadow bits are not exercised there (menus5's loading
  observe covers them). A real load is too short for a forced model to show (`s7-blood-obs3-set`: the cocs' loading screens ended before
  the models loaded).

## Detector (`[EngineZones]`, native Vulkan; Dev-Fast)

| Run | Main-menu frames | World frames |
|---|---|---|
| before: `s7-book-fin-nvk-book` (base code) | - | fatal at a loading screen with the blood model |
| sweep, owned (`s7-blood-sw2-ls`, 266 models, 10,888 main-menu frames) | Main::DrawFrame only (1.0 per frame); UI3DSceneManager::Render never entered | - |

## Decided by the agent, to confirm

- **A Soft menu Effect reads the null view at t3**, as D3D11's hazard rule leaves the engine's draw (kMAIN's depth SRV while that depth
  is bound for writing).
- **The engine's stale pass lists are not reproduced** (shadow bits, light lists, DoAlphaTest by early-Z): CS computes them each render.
- **A lit Effect's lights are `SelectLights`'** (the Lighting rule); observed only with the stale list above.
- **The loading model sweep runs at the main menu** (keep depth off); a real load does not stay long enough to show each model.
- Particle PreviousWorld = World (SetupGeometry's copy), fixed although outside the item.

## Proposed decision-log entries

- Closes 203's blood and partition items: the blood Effect variant (AlphaTestRef from the threshold and the weapon blood globals), Soft
  Effects (camera rows, far * near / softFalloffDepth, t3 null as D3D11 unbinds it) and lit Effects (light rows from SelectLights) are
  computed by the native menu scene; skinned shapes without renderer data draw one draw per drawn partition with the whole skin's
  palette. `loadscreens` and `menu-loadscreens` show all 266 loading models: owned native passes, observe computes every draw.

## Open items

1. Observe only compares draws whose engine capture succeeds; a draw the capture refuses (a published texture, a streaming texture) is
   not counted as computed or not. The owned sweep is the check that matters.
2. The engine's cached pass lists (above) if parity in the first frames after a model change matters.
3. The local map and deleting `CS_MENU_SCENES_OBSERVE` (snap5-map's).

## Runs (Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-vulkan+menu-loadscreens, observe | s7-blood-obs1-ls | passed; blood computed; observe missed Soft draws (capture refused t3) |
| native-vulkan+menu-loadscreens, owned | s7-blood-sw1-ls | hang: fatal at `p_Dust` (Soft particles, pass 0x410c7) |
| native-vulkan+menu-loadscreens, observe | s7-blood-obs2-ls | passed; differences above |
| native-vulkan+menu-loading-set (scratch: four forced models in real loads), observe | s7-blood-obs3-set | passed; only the first load showed its model |
| native-vulkan+menu-loadscreens, owned | s7-blood-sw2-ls | passed; 266 models, no fatal |
| native-vulkan+menu-loadscreens (MaleOrc_1 only, 300 frames) | s7-blood-sw3-orc | passed |

Final runs (`s7-blood-fin-*`, on d89bc4e6a's code), all passed: native-vulkan+menu-loadscreens (detector: main-menu frames only Main::DrawFrame,
266 models), native-vulkan+quick, native-vulkan +menu-loading (1 engine render call per world frame), +menu-scenes (1), +menu-book (4: the
page copies), +menu-items, +menu-main. A stopped batch script kept running beside the trimmed one, so these also ran (and passed):
guest+menu-loadscreens, guest+quick, native-d3d12-quick, guest+menu-loading.

Skipped (director's test-scope rule, 2026-11-27): guest +menu-scenes, +menu-book, +menu-items, +menu-main and native-vulkan+cadence-drop-cells.
The change is the native menu scene's computation (owned native only; guest never runs it), the observe/capture path (native observe
only) and a new test command; nothing touches cadence, ORG, BasicRHI or the native device.

Shot references: none to accept (`menu-loadscreens` keeps no references: the last model's shot depends on the model order).
