# snap5-menus3: menu scenes, third round (roadmap phase 5, "Menu scenes")

Lead `menus3`, branch `snap5/menus3` (base `org_integration` 8d21e6028), stage `s7-menus3`. Starts from
[snap5-menus.md](snap5-menus.md) and [snap5-menus2.md](snap5-menus2.md); decisions 146-150, 160, 167-169.

## State

The item is **not finished**. MistMenu's main menu is switched: CS computes and draws every draw of a MistMenu render without a depth
pass (the main menu's logo, its glow and the smoke particles) and stages the menu HDR chain with its own rows; the engine's
UI3DSceneManager::Render and RenderEndOfFrameEffects are not entered at the main menu. MistMenu's loading model (renders with a depth
pass and shadow maps), the inventory, lockpicking, book, journal and the local map are still the engine's.

| Item | State |
|---|---|
| 1. MistMenu, main menu | done: particles, light rule, menu HDR, fog, sun scale, menu lights; owned; main-menu frames 20 -> 1 engine render call (Main::DrawFrame) |
| 1. MistMenu, loading model | not done: needs the depth pass, the shadow maps and the shadow mask (RE below); its renders stay the engine's |
| 2. Inventory | not done: needs the membrane pass (RE below), skinned Effect draws; config `menu-items` added (enchanted weapon, enchanted helmet) |
| 3. Lockpick, journal, book, local map | not started |
| 4. Delete `CS_MENU_SCENES_OBSERVE` | not done (schemes left) |

## What was built

All in `Features/MenuViews/MenuScenesNative.cpp` unless named.

- **Particles** (the main menu smoke, `PArray01`). `ComputeEffect` covers a particle system's pass (Particles, with TexCoordIndex): its
  World and PreviousWorld fold the model bound's centre into the translation, as SetupGeometry does. `AddParticles` expands the system's
  state, read right after the update half, faced to the menu camera, with DCLF's particle expansion (`ParticleSnapshots::ExpandNow`, a
  new entry beside `Expand`; the slot fill and the expansion core are shared, `FillSlot` and `ExpandSlot`), and leases the renderer's quad
  index list. The quads go in the draw's latch region after its constants and indexed texture coordinates (row 0 the identity rect, the
  rects being resolved per particle). `Effect.hlsl`'s ORG build pulls them by the vertex's index when the draw has no stream 1
  (`OrgParticleInput`; the corner decode is shared with DCLF's pull, `ParticleCornerInput`). A latched particle draw still pulls stream 1.
- **The shadow-light rule.** `Inputs::shadowMaps` (scheme 4 with a depth pass, the accumulator's +0x178 in the engine): only then do a
  pass's shadow lights add their descriptor bits. Owned renders never have it.
- **Menu HDR** (`ImageSpace::MenuSceneHdr`, `NativePost.inl`: `MenuData`, `MenuHdrSteps`, `RunMenuHdr`). With an override (manager
  +0xB0) the native render stages the menu composite's HDR steps with rows from the world HDR effect's code (`HdrSteps`) on the menu's
  data: the override as the engine's copy and finish make it (their float operations ported), the adaptation reset, the framebuffer
  range 1, through the tonemap call site as the world's. `TakeMenuStep` is split so `StageMenuKey` stages a menu key with native rows.
- **Inputs fixed while switching** (observe could not see them: it samples after the engine's own setup):
  - the menu render processes its queued lights on `State.shadowSceneNode[1]`, not `[0]` (the logo had no point lights);
  - it sets the menu culling process's camera and frustum before the light update, which dims lights by them (the shadow light's
    `lodDimmer` was 0);
  - the shadow lights' LOD dimmers are the shadow render's (`ShadowLightDimmers`): MistMenu's shadow render (`FUN_140973210`) sets each
    shadow light's dimmer in its camera update (vtable +0x80), visibility in the menu camera's frustum and the shadow fade distance; CS
    draws no shadow map for an owned render but computes the dimmer the light rows read (the main menu's warm key light was 0);
  - the fog is the menu scene node's (`SampleFog`, split out of `SampleTechniqueInputs`);
  - DirLightColor scales by the override's HDR multiplier in MistMenu's scenes (`SamplePipelineFrame`'s new `a_hdr`).
- **Ownership** is per render: `Owns(scheme, keepDepth)`; scheme 4 is owned without a depth pass. The detector's `NativeOwns` uses each
  scheme's last render (`NoteRender`). Observe builds own nothing.
- **Observe** additions: menu HDR rows per step against the engine's (`ObserveMenuHdrStep`, compared at the engine's menu steps).

## RE notes (AE 1.6.1170)

### UI3DSceneManager::Render (AE 52727, 0x140972590) as MistMenu calls it

- MistMenu::PostDisplay (AE 52308) sets the directional ambient from the menu's own (`FUN_141480340(this + 0x58)`), calls
  `Render(manager, 4, null, keepDepth)` and restores the ambient. keepDepth = the loading model is up and
  `iLoadingMenuShadowLightFlags` (AE 383327, `0x1420187f8`) & ~8 is at least 1. The main menu's logo has keepDepth false.
- The menu scenes' scene graph is the shader manager's second: `State.shadowSceneNode[1]` (AE 390952, `0x142033068`; the State is
  AE 390951). The render processes its light queues there (AE 106335 on `shadowSceneNode[1]`), and sets the colour accumulator's
  +0x178 to `scheme == 4 && keepDepth`. `BSLightingShaderProperty::GetRenderPasses` (0x1414adfb0) reads +0x178: only with it set do a
  pass's shadow lights add their descriptor bits; otherwise they are plain lights of the pass (counted first in its light rows).
- Scheme 4 with `iLoadingMenuShadowLightFlags` non-zero calls `FUN_140973210` (shadow maps) whatever keepDepth is: for each of
  `shadowSceneNode[1]`'s active shadow lights (at most four) that pass `UpdateCamera` (vtable +0x80; parabolic 0x14151b620, frustum
  0x14151ac50) it assigns a shadow map index (+0x520) and a viewport, then renders its shadow map (vtable +0x48: parabolic 0x14151b960,
  frustum 0x14151aa60); the others get vtable +0x60. At the main menu nothing samples these maps.
- With keepDepth, a depth pass (accumulator +0x18, `StartAccumulating` flags | 0x20) into kMAIN's depth, then `FUN_140972f70`: per shadow
  caster light a full-screen Utility pass (`(0x800000 | 0x400000 | 0x1000000 by light kind) | 0x2002) + 0x2B`) into kSHADOW_MASK with the
  write mode of the light's index. Then the colour pass.
- The colour pass draws into kMAIN (scheme 4) or the back buffer. Then, with an ImageSpace override (manager +0xB0):
  `FUN_141482ba0` copies the override into the current data (+0xC8; clear `0x1414cab10`, copy `0x1414cab40`, clear), `FUN_141482c90`
  finishes it (`0x1414cabb0`), `FUN_1414d13c0` resets the adapted luminance (391302), and `RenderEndOfFrameEffects(manager, 1, 0)` runs
  the HDR effect from kMAIN into the back buffer. Around the draws the render swaps 414582/414583 for constants: the bloom blur's range
  rows read 1, 1 in a menu.

### Membrane passes (the inventory's enchantment glows)

- `BSLightingShaderProperty::GetRenderPasses` (0x1414adfb0, end) adds a BSEffectShader pass when the property's `effectData` (+0x68,
  `BSEffectShaderData`) is set and the property is not refraction (flag bit 17): descriptor `0x8042` (Membrane, Texture, TexCoord) | Vc
  (flags bit 37) | Skinned (vertex desc & 2) | Normals and BinormalTangent (0x30) unless model-space normals (vertex desc & 0x1000) |
  GrayscaleToColor (data +0x7D) | GrayscaleToAlpha (+0x7E) | IgnoreTexAlpha (+0x7F) | ProjectedUv (+0x80) | AlphaTest (0x800000: not
  +0x81 and the alpha property tests) | Lighting (+0x82) | AddBlend (0x400) when destBlend (+0x74) is 0; pass enum + 0x4000002C. Its
  accumulation hint is 1 (blended list) when the property's fade times alpha is below 1, else 3. A block-out texture (+0x20) adds a
  Utility pass (hint 0xE) and moves hints 0, 3 and 15 to 0x10.
- `BSEffectShader::SetupMaterial` (0x1414ea6c0) for a membrane writes no constants; the material is the geometry's Lighting material:
  t1 = its normal map (+0x58) unless the pass has Normals, address 3 filter 3; t2 = its diffuse (+0x48) with AlphaTest, address 3 filter 3.
- `BSEffectShader::SetupGeometry` (0x1414eab10) calls the membrane setup `0x1414ebbe0`: PS MembraneVars.x = edgeExponent (+0x68);
  PropertyColor = fillColor (+0x2C) times baseFillScale (+0x4C), or fillColor with MembraneVars.z = baseFillScale under
  GrayscaleToColor; MembraneRimColor = rimColor (+0x3C); VS World and PreviousWorld (unskinned only); TexcoordOffsetMembrane = u/v offset
  and scale (+0x58); EyePosition = the current accumulator's eye (+0x16C) through the inverse world; PosAdjust = posAdjust, then x =
  fade node +0xEC - +0xF0 and y = 2 * +0xF0 (a global pair in render mode 0x16); t0 = baseTexture (+0x10) address 3; t4 = paletteTexture
  (+0x18) under GrayscaleTo*, address 0 filter 1; AlphaTestRef (with AlphaTest) = alphaTestRef byte / 255 + bias; blend mode from
  src/dest blend (+0x70/+0x74: 6/7 -> 1, 6/0 or 6/9 -> 2, 4/7 -> 3, 1/2 -> 4); depth mode 1 and write mode 1 unless the pass is in a
  motion-vector render.
- CS has no membrane port anywhere (DCLF's Effect material port refuses membranes). The menu path needs the pass list (the property's
  extra pass), the setup above and the blend states, then observe against the latched membrane draws.

## Runs (native Vulkan unless named; Dev-Fast)

| Config | Label | Result |
|---|---|---|
| menu-main, observe + detector | s7-menus3-obs1-main | passed; particles 0 differences (but the first frame's PreviousWorld); shadow-light rule 0 of 600 pass descriptors; menu HDR rows: blur range rows differ (fixed) |
| menu-main, scheme 4 owned | s7-menus3-sw1-main ... sw6-main | passed; logo missing, then black, then cool-tinted: the input fixes above in turn (fog, menu lights on `shadowSceneNode[1]`, culler, dimmers) |
| menu-main, observe (dumps) | s7-menus3-obs2-main, obs3-main | passed; light values and full rows for the comparisons; menu HDR rows 0 differences in 4000 steps |
| menu-main, scheme 4 owned, detector | s7-menus3-sw7-main | passed; logo matches the engine-drawn shot (crop mean 56.1 vs 55.7); smoke within the engine's own shot-to-shot variance; main_menu 1.0 engine render calls per frame |
| menu-main, observe | s7-menus3-obs4-main | passed; no shadow light dimmer or light differences logged; menu HDR rows 0 differences |
| native-vulkan+menu-main (detector) | s7-menus3-fin-nvk-main | passed; main_menu 1.0 per frame (was 20), MistMenu site producer |
| native-vulkan+menu-scenes, +quick, native-d3d12-quick | s7-menus3-fin-nvk-scenes, -fin-nvk-quick, -fin-d3d12-quick | crashed at the loading screen (the shadow mask crash above); loading frames then made the engine's |
| guest+menu-scenes, guest+quick | s7-menus3-fin-guest-scenes, -fin-guest-quick | passed |

## Decided by the agent, to confirm

- **MistMenu is owned per render kind:** a scheme 4 render without a depth pass outside a load (the main menu) is CS's; one with a depth
  pass (a loading model with shadow lights) stays the engine's until its depth pass, shadow maps and shadow mask are ported, and so does
  every MistMenu render in a loading frame (the loading loop's thread). Decision 149 is kept per render: every draw of an owned render is
  computed, and anything else in it is fatal. The detector reads each scheme's last render.
- **Why loading frames stay the engine's:** with CS drawing the loading screen's first MistMenu renders (no model yet) and the engine the
  later ones (the model, with its depth pass), the engine's shadow mask call crashed: DCLF's shadow mask path (`ShadowMaskHooks.cpp`,
  `MaskBatch`) applied the pending state with the vertex shader dirty and none bound (`FUN_140e4b7d0` +0x40B), in
  `native-vulkan+menu-scenes`, `+quick` and `native-d3d12-quick` (`s7-menus3-fin-*`). The cause was not found; the next lead porting the
  loading model owns the whole loading screen and meets it there.
- **The update half sets the menu culling process's camera and frustum** before the light update (an engine CPU call, as decision 147's).
  The engine's light update dims lights by the planes its last cull left; CS does not cull with that process.
- **The shadow lights' dimmers are computed, not their shadow maps:** at the main menu the engine renders shadow maps nothing samples
  (no pass has shadow bits without the depth pass); CS ports only the camera update's dimmer, which the light rows read.
- **The menu HDR reads the override at +0xB0 directly** and does not copy it into the manager's current data: the engine's copy (a write of
  game-visible ImageSpace data from the render half) is not made. DirLightColor in MistMenu's scenes takes the override's HDR multiplier,
  which is what that copy would have left there; over a world the game's blend replaces it each update, so other schemes keep the current.
- **The framebuffer range in the menu HDR is 1** (observed rows); the swapped globals were not identified further.
- **The light fade distances the engine swaps around its render (AE 414582/414583 -> 1e6, 2.25e6) are not swapped:** they only fade
  point lights farther than their radius from the camera, which no menu light is. A menu with such a light would differ.

## Proposed decision-log entries

- The main menu (MistMenu renders without a depth pass) is CS's: particles from DCLF's expansion in the draw's latch, the menu HDR chain
  with CS's rows from the override (the world HDR code), the menu scene node's fog and lights (`State.shadowSceneNode[1]`), shadow lights
  as plain lights without shadow maps and with the shadow render's dimmer. Main-menu frames: 20 -> 1 engine render call (DrawFrame).
- A loading model's MistMenu render (depth pass, shadow maps, shadow mask) stays the engine's; its RE is in snap5-menus3.md.

## Open items, in order

1. **Loading model** (scheme 4 with a depth pass): the depth pass (Utility RenderDepth into kMAIN's depth), the shadow maps per active
   shadow light (the camera update and Render of the light's class, vtable +0x80/+0x48; DCLF has no menu shadow path), the shadow mask
   (`FUN_140972f70`) and the Lighting draws' shadow bits and mask reads. Then own scheme 4 whole.
2. **Inventory** (schemes 1 and 2): the membrane pass (RE above), skinned Effect draws (`ComputeEffect` refuses Skinned; palettes exist),
   particles (now covered). Run `native-vulkan+menu-items` with `CS_MENU_SCENES_OBSERVE=1` (pass the config by path) to observe them.
3. Lockpick, journal, book (Scaleform page target import), local map.
4. Delete `CS_MENU_SCENES_OBSERVE` with the last scheme.
