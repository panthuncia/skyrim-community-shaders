# snap5-menus4: menu scenes, fourth round (roadmap phase 5, "Menu scenes")

Lead `menus4`, branch `snap5/menus4` (base `org_integration` 889d489c6), stage `s7-menus4`. Starts from
[snap5-menus.md](snap5-menus.md), [snap5-menus2.md](snap5-menus2.md) and [snap5-menus3.md](snap5-menus3.md); decisions 146-150,
167-169, 183-184.

## State

The item is **not finished**. The inventory (schemes 1 and 2) is switched: CS computes every draw of an inventory item, its
enchantment membranes included, and the engine's UI3DSceneManager::Render is not entered for it. MistMenu renders without a depth
pass are now CS's in loading frames too (a loading screen before its model), and the shadow-mask crash that kept them the engine's is
found and fixed. The loading model with its depth pass, shadow maps and shadow mask, lockpicking, the book, the journal and the local
map are still the engine's, so `CS_MENU_SCENES_OBSERVE` stays.

| Item | State |
|---|---|
| 1. Inventory (schemes 1, 2) | done: membrane pass, skinned Effect draws allowed, owned; shots match the engine-drawn native |
| 2. Loading frames | partly: renders without a depth pass owned in loading frames, crash cause found and fixed; the loading model (depth pass, shadow maps) not ported |
| 3. Lockpick, journal, book, local map | not started (no cs-test step can open lockpicking or a book yet) |
| 4. Delete `CS_MENU_SCENES_OBSERVE` | not done (schemes left) |

## What was built

All in `Features/MenuViews/MenuScenesNative.cpp` unless named.

- **Membrane pass** (`MembranePassOf`, `ComputeMembrane`, `MembraneStateOf`, `AddMembrane`). A Lighting property with effect data
  gets a second pass, computed from the RE below: its descriptor from the property flags (skinned bit 1, model-space normals bit 12,
  vertex colours bit 37; refraction, bit 17, adds none) and the effect data; the Effect technique rows (shared with `ComputeEffect`
  through `EffectTechnique`); no material constants; the Lighting material's normal map (t1, without Normals) and diffuse (t2, with
  AlphaTest), the effect data's base texture (t0) and palette (t4); World/PreviousWorld (unskinned), EyePosition (the eye in model
  space), TexcoordOffsetMembrane, PosAdjust (fade node bound bottom and height, eye z), PropertyColor (fill times base fill scale,
  with Linear Lighting's conversion), MembraneRimColor, MembraneVars, AlphaTestRef (threshold times 0x3B808081 plus 0x3B80802C, the
  exe's constants). States: the effect data's blend pair (6/7 -> 1, 6/0 or 6/9 -> 2, 4/7 -> 3, 1/2 -> 4), depth test without write,
  depth bias mode 10 (11 with the accumulator's +0x51), back faces culled, write mode 11, alpha test off. Lit and projected-UV membranes
  and block-out textures are refused (fatal in an owned scheme; none seen).
- **Draw order**: `SceneGeometry` returns passes (`ScenePass`). Membranes form their own group after the batches and before the
  blended list, as FinishAccumulating (AE 0x1414b3f20) draws group +0x90 (FUN_1414b3d50) there.
- **Depth bias** in native menu draws: `DrawState::depthBias` goes to the raster state and takes its offset off the viewport's
  maximum depth (`Family::DepthBiasViewportOffset`, a small additive helper in `EngineDrawFamily`).
- **Skinned Effect draws** are no longer refused; a skinned pass writes no World (its palettes place it), as SetupGeometry.
- **Streaming textures**: a draw whose texture is still streaming in (`ResolveFailure::Pending` or `StreamedWindow`) is left out of
  that render and logged once, like 169's building programs and 184's unbuffered geometry. Chillrend's first frames need it.
- **Ownership**: `Owns` adds schemes 1 and 2, and scheme 4 without a depth pass in any frame kind.
- **Shadow mask guard** (`ShadowMask/ShadowMaskHooks.cpp`, `MaskBatch`): see the crash below.
- **Observe**: membrane passes compared under their own fields (`membrane vs geometry`, `membrane ps geometry`, textures, samplers,
  shader descriptors, states and accumulation hint 3). `Observe::Effect` takes the engine's bound views and sampler modes.
- `menu-items.toml` opts into shot references (both shots).

## The shadow-mask crash (menus3's open item)

Cause: the first engine-drawn MistMenu render in a process whose earlier renders were all CS's (a native main menu, then the loading
screen's renders before its model). The menu mask hook (`MaskBatch<View::Menu>`) applies the pending state to land its clear. The
engine's state then has `DIRTY_VERTEX_DESC` set and no vertex shader ever bound (nothing engine-side has drawn since start-up), and
`SetDirtyStates` reads the null shader's vertex description (AE 0x140e4b9bb, `mov rbx, [rax+0x48]`, menus3's crash log). Fix: when no
vertex shader is bound, the vertex layout bit is cleared before the apply (there is no layout without a shader; the next technique sets
it again), logged once. Confirmed: `s7-menus4-ld1-main-load` and `-ld1-scenes` logged the case and passed.

## RE notes (AE 1.6.1170)

- Membrane pass descriptor: BSLightingShaderProperty::GetRenderPasses (0x1414adfb0) end; `uVar9` there is the property's flag word,
  not the vertex description (menus3's note said vertex desc).
- BSEffectShader::SetupTechnique (0x1414ea2c0): fog, FilteringParam; a Texture pass sets t0's filter to 3.
- SetupMaterial (0x1414ea6c0) membrane branch: t1 = material +0x58 (normal) without Normals, t2 = +0x48 (diffuse) with AlphaTest,
  both address 3 filter 3.
- Membrane setup (0x1414ebbe0) as listed above; it sets depth mode 1, bias mode 1 only outside hint 2/3 passes, and always culls back
  faces and turns the alpha test off. SetupGeometry writes mode 11 for hint 2/3.
- FinishAccumulating (0x1414b3f20): main batches, group +0xb0, the decal groups (FUN_1414b3bb0: bias 6/8 plus the accumulator's +0x51
  when depth bias is on), group +0x90 with write mode 11 (FUN_1414b3d50: depth mode 1, bias 10 plus +0x51), then the blended list.
- Inventory3DManager::Render (AE 51755), BookMenu::PostDisplay (51051) and JournalMenu::PostDisplay (53317) reach
  UI3DSceneManager::Render by a tail jump, so the detector has no call site for them; their inner sites count.
- The menu colour accumulator is the manager's +0x10 (it gets the camera's eye at +0x16C and +0x178 for shadow maps).

## Detector (`[EngineZones]` exit lines, native Vulkan)

| Config | Before | After |
|---|---|---|
| menu-items, world frames | 20.6 per frame (`s7-menus4-obs1-items`, nothing owned; the inventory's inner sites in 526 of 735 frames) | 9.0 (`s7-menus4-sw4-items`) |
| menu-scenes, world frames | 14.7 (`s7-menus3-m4-nvk-scenes`) | 9.0 (`s7-menus4-ld1-scenes`) |
| menu-scenes, loading frames | 11.1 (372 of 639 frames with any) | 11.8 (330 of 532): the model renders remain |
| menu-main-load, loading frames | 6.4 (98 of 293 frames with any, `s7-menus3-m4-main-load`) | 4.2 (94 of 426) |
| main-menu frames | 1.0 | 1.0 |

The loading frames left are the loading model's renders (depth pass, shadow maps), about 20 calls each.

## Decided by the agent, to confirm

- **Every menu membrane is drawn in the membrane group (hint 3).** GetRenderPasses' decompile reads hint 1 when the fade node's fade
  is below one, but the menu models' fade is 0 and the engine gave all 76 observed membranes hint 3; the decompile's fade variable is
  not trustworthy there. Observe checks the hint.
- **A draw whose texture still streams in is left out of that render**, as 169 and 184 (a transient; the engine draw families leave
  the batch for a later one).
- **The shadow mask hook clears the vertex layout bit when no vertex shader is bound** before applying the engine's state.
- PosAdjust.z is the eye's z (the engine's posAdjust global under CS's world-space patch); it matched in observe.

## Proposed decision-log entries

- The inventory (schemes 1 and 2) is CS's: the membrane pass computed from the Lighting property's effect data (descriptor, rows,
  textures, membrane group states with depth bias 10), skinned Effect draws, draws whose textures still stream in left out of that
  render. World frames with the inventory open: 20.6 -> 9.0 engine render calls.
- MistMenu renders without a depth pass are CS's in loading frames too. The shadow-mask crash was the first engine-drawn menu mask
  in a process with no vertex shader ever bound (its pending vertex layout read the null shader); the hook clears that bit first.

## Open items, in order

1. **Loading model** (scheme 4 with keepDepth): a Utility depth pass into kMAIN's depth, the shadow maps per active shadow light of
   `shadowSceneNode[1]` (UpdateCamera vtable +0x80, Render vtable +0x48: BSShadowParabolicLight::Accumulate 0x14151b960 and the frustum
   one; menu geometry is not in DCLF's scene, so this needs a Utility shadow-depth path for menu geometry), then the existing native
   menu masks and the Lighting shadow bits. Then own scheme 4 whole; loading frames should drop to about 0.
2. Lockpicking (scheme 7): needs a test step that opens it (a locked reference with a known form id). Book (3): Scaleform page
   target import. Journal (scheme 0 with flag 0x80): no 3D scene seen in runs. Local map.
3. Delete `CS_MENU_SCENES_OBSERVE` with the last scheme.

## Runs (native Vulkan unless named; Dev-Fast)

| Config | Label | Result |
|---|---|---|
| menu-items, observe + detector | s7-menus4-obs1-items | passed; membranes 0 differences but AlphaTestRef by 1 ulp (fixed with the exe's bias constant); membrane states 0 of 76 |
| menu-items, owned | s7-menus4-sw1-items, -sw2-items, -sw3-items | hang: fatal on a streaming texture, then on the fade rule (both fixed) |
| menu-items, owned, detector | s7-menus4-sw4-items | passed; both shots match the engine-drawn `obs1` (ref check pass); 9.0 per world frame |
| menu-main-load, detector | s7-menus4-ld1-main-load | passed; mask guard logged once; loading 4.2 |
| menu-scenes, detector | s7-menus4-ld1-scenes | passed; shot references pass; world 9.0, loading 11.8 |

Final runs (`s7-menus4-fin-*`, on d0edb4913's code): guest+quick, native-vulkan+quick, native-d3d12-quick, guest+menu-scenes,
native-vulkan+menu-main (main menu 1.0 per frame), native-vulkan+facegen passed. native-vulkan+cadence-drop-cells timed out: the main
checkout's config already needs snap3-cocwait's step change, which this base lacked. After merging org_integration (a5f868f4f):
native-vulkan+cadence-drop-cells, native-vulkan+menu-items and native-vulkan+quick (`s7-menus4-m1-*`) passed.

Shot references to accept: `s7-menus4-m1-native-vulkan-menu-items` (both inventory shots; checked against the engine-drawn
`s7-menus4-obs1-items`).
