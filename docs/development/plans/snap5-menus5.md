# snap5-menus5: menu scenes, fifth round (roadmap phase 5, "Menu scenes")

Lead `menus5`, branch `snap5/menus5` (base `org_integration` c224bd46f), stage `s7-menus5`. Starts from
[snap5-menus.md](snap5-menus.md) to [snap5-menus4.md](snap5-menus4.md); decisions 146-150, 167-169, 183-188.

## State

The item is **not finished**. The loading model (MistMenu, scheme 4 with its depth pass) and the lockpicking menu's lock (scheme 7)
are switched: CS computes and draws every draw of them, the engine's UI3DSceneManager::Render is not entered for them, and loading
frames read 0.0 engine render calls (11.8 before). The book (scheme 3) and the local map are still the engine's, so
`CS_MENU_SCENES_OBSERVE` stays.

| Item | State |
|---|---|
| 1. Loading model (shadow maps, depth pass, masks, shadow bits) | done: owned; loading frames 11.8 -> 0.0; shot matches the engine-drawn native |
| 2. Lockpick (scheme 7) | done: test command and config, observe 0 differences, owned; shot matches the engine-drawn native |
| 3. Book (scheme 3) | not done: native already stops at its page copy (below); journal: no 3D scene (as menus1 saw) |
| 4. Local map | not started |
| 5. Delete `CS_MENU_SCENES_OBSERVE` | not done (the book is left) |

## What was built

- **`MenuScenesShadows.cpp`** (new), `RenderShadowsAndDepth`: UI3DSceneManager::Render's loading-model work before its colour pass,
  in its order, with no engine render function:
  - FUN_140973210's light loop (AE 0x140973210) through the native shadow ports: `ResetKept` (FUN_1414a4030), the first four active
    shadow lights whose `Shadows::Native::UpdateLightCamera` passes are kept (a one-slot keep without the world's frame driver call),
    each with the next mask channel, `ProjectLightRect` and `AccumulateBookkeeping`; the others `ReturnShadowmaps`. The portal call
    FUN_1414a2530 is not made (the menu node has no portal graph; it only builds the light's own geometry list, which CS does not read).
  - Each kept light's Render bookkeeping through `Shadows::Engine::RenderMenuLight` (new, `ShadowMapsNative.cpp`: the world port's
    `Render` without the camera state tail: targets, free-mask slices, the slice clears and the light transforms), then its maps:
    each descriptor's camera draws the menu geometry it sees (`ComputeUtility` in its accumulator's render mode) into its slice; a
    paraboloid's back hemisphere draws into the front's slice, both take every caster in the light's sphere (the parabola clip drops
    the other side).
  - The depth pass into kMAIN's depth (render mode 0xC techniques, no colour target), then `ShadowMask::DrawMenuMasks` (new, the
    world's `DrawWorldMasks` shape: kSHADOW_MASK cleared, `Native::Build(View::Menu, ..., &eye)` with the menu camera's eye, CS's own
    start modes and the adopted mask size).
- **`ComputeUtility` / `AddUtility`** (`MenuScenesNative.cpp`): a menu model's Utility draw from BSUtilityShader's SetupTechnique,
  SetupMaterial and SetupGeometry (AE 0x1414fa160, 0x1414faa60, 0x1414fae40): technique from `ShadowUtilityTechnique` and
  `ShadowModeBits`, vertex descriptor by `ShadowMask::Native::VertexDescriptorOf` (now public), ParabolaParam, TexcoordOffset and
  t0 (diffuse, the material's clamp mode, filter 3), World less the camera's eye (unskinned), the depth pass's AlphaTestRef (0x3F7EFEFF
  while blending, else threshold * 1/255 + the exe's bias), frame rows from the camera (`UtilityFrameRows`). The depth pass takes a
  Lighting property that writes depth and is not blended or a decal; a shadow map its `ShadowCasterReject` casters.
- **Colour pass with shadow maps**: the shadow bits also go into the raw pixel descriptor (DefShadow reaches the shader through CS's
  permutation block), ShadowLightMaskSelect (PS c2) gets each shadow light's mask channel (the point light routine, AE 0x1414df650),
  and t14 is the shadow mask, read by its published id (`AddTexture` reads published targets now).
- **Alpha test reference**: threshold * 0x3B808081 (the exe's 1/255), as the batch renderer sets it; was threshold / 255 (1 ulp off).
- **Ownership**: `Owns(scheme)` holds 1, 2, 4, 6 and 7 whatever the depth pass flag; `NoteRender`, `LastKeepDepth` and the flag are
  deleted. `DrawState` carries the stencil mode and reference.
- **Observe**: `Observe::Utility` compares every latched depth-pass and shadow-map draw (technique, rows, alpha reference, states,
  viewport, frame rows, t0) and the depth pass's membership; the shadow batch now opens an observe scope.
- **Test harness**: `lockpick` (locks the nearest door or container, gives a lockpick, activates it as the player) and `book` (opens
  the first plain book through BookMenu::OpenMenuFromBaseForm) commands in `Testing/Commands.cpp`; configs `menu-loading` (one load
  screen forced with `loadscreen` before the save's load: a comparable loading model shot) and `menu-lockpick`. Pass both by path.

## Observe results (native Vulkan)

`s7-menus5-obs1-scenes` (a robed NPC loading model, skinned, with a paraboloid shadow light) and `-obs2-loading` (Alduin's wall):
- menu-depth: technique, vs/ps rows, states, t0: 0 differences. Left, explained: the frame's c20-c23 by 1 ulp (decision 148), the
  engine's stale 0.999998 depth range (148), membership in 3-4 batches cut short by a streaming refusal (the engine drew the rest).
- menu-shadow: technique, rows, frame, viewport, t0: 0 differences after the fixes. Left: the write mode (the engine's stale 1 or 12
  with no colour target bound; CS uses 0).
- Lighting with shadow maps: pass descriptors, t14 (view, address 0, filter), ShadowLightMaskSelect, VPOSOffset: 0 differences;
  the permutation descriptor differed (DefShadow) until fixed. Skinned Lighting draws' World rows differ (the engine leaves stale rows
  a skinned pass does not write); not read.
- Lockpicking (`-obs3-lockbook`): every Lighting draw 0 differences; states as the inventory's (the engine leaves RT1 = motion vectors
  bound from the world; CS draws the framebuffer only, decision 168).

## The book (open)

`s7-menus5-obs3-lockbook`: opening a book in native mode (observe build, nothing owned) stops the game: "frame tap error ...
BSImagespaceShaderCopy draw has no CS pass (into rt105, reads t0 a loaded texture)". rt105 is kBOOK_TEXT_0: BookMenu copies its
page background into the book text target with an ImageSpace copy, Scaleform draws the page text into that target, and the book
model's Lighting draws sample it. Native needs, in order: the copy as a UI-stage step (`MenuSteps.inl`), the native Scaleform
renderer drawing a display into kBOOK_TEXT_n (today `SetDisplayRenderTarget` to a target without a view is left out), kBOOK_TEXT_n
as a published input of the menu colour client (`ListInputs`), then observe scheme 3 and own it. This is pre-existing: the book
crashes native whatever the menu scene ownership.

## Detector (`[EngineZones]` exit lines, native Vulkan; Dev-Fast)

| Config | Before | After |
|---|---|---|
| loading frames, menu-scenes | 11.8 per frame (`s7-menus4-ld1-scenes`) | 0.0 (2 of 636 frames with any; `-fin-nvk-scenes`) |
| loading frames, menu-loading | (engine-drawn observe run `-obs2-loading`: model renders latched) | 0.0 (3 of 590 frames with any: Main::DrawFrame; `-sw1-loading`) |
| loading frames, menu-main-load | 4.2 (`s7-menus4-ld1-main-load`) | 0.0 (2 of 318 frames with any; `-fin-nvk-main-load`) |
| world frames (menu-scenes, menu-items), main-menu frames | 9.0, 1.0 | 9.0, 1.0 (unchanged) |
| lockpicking frames | UI3DSceneManager::Render (LockpickingMenu) [render] and its inner sites | the site reads producer; world 9.0 per frame (`-sw3-lockpick`) |

## Decided by the agent, to confirm

- **A paraboloid light's two hemispheres both draw every caster in its sphere**; the shader's parabola clip drops the other side.
  The engine's back-hemisphere cull is not ported (the result is the same, with more draws).
- **Shadow map draws use write mode 0** (no colour target is bound; the engine's mode is whatever the last draw left).
- **The menu masks clear kSHADOW_MASK to white**: each kept light writes its own channel, the only one its passes read.
- **The menu's portal light call (FUN_1414a2530) is not made**: it builds the light's geometry list, which CS's light reach replaces.
- **`ProjectLightRect`'s camera-inside bit** (a world global, reset at each world selection) is set by menu lights too, as the port
  writes it; nothing reads it for the menu.

## Proposed decision-log entries

- The loading model (MistMenu with its depth pass) is CS's: FUN_140973210's light loop through the native shadow ports, each map and
  the depth pass as Utility draws CS computes from BSUtilityShader's setup, the menu masks built natively with the menu eye, and the
  Lighting draws' DefShadow, ShadowLightMaskSelect and t14. MistMenu is owned whatever the depth pass flag. Loading frames: 11.8 -> 0.0
  engine render calls.
- The lockpicking menu (scheme 7) is CS's (observe 0 differences); `lockpick` and `book` test commands.
- The book stays the engine's: native stops at BookMenu's page copy into kBOOK_TEXT_0 (an ImageSpace copy with no CS pass); the copy,
  the Scaleform page target and the target import are its open work.

## Open items, in order

1. Book (scheme 3): the page copy step, the Scaleform display into kBOOK_TEXT_n, the target import, observe, own.
2. Local map: its own native path (LocalMapRenderer::Render: the map camera, the land and objects, the fog of war, ISLocalMap).
3. Delete `CS_MENU_SCENES_OBSERVE` with the book's switch.

## Runs (native Vulkan unless named; Dev-Fast)

| Config | Label | Result |
|---|---|---|
| menu-scenes, observe + detector | s7-menus5-obs1-scenes | passed; differences above, fixed |
| menu-loading (first form), owned | s7-menus5-sw1-loading | passed; loading 0.0 per frame; the coc's loading shot fell in the fade (config changed) |
| menu-loading, owned | s7-menus5-sw2-loading | failed in the config's last step (waited for a second world-ready; config fixed); loading-model shot drawn |
| menu-loading, observe | s7-menus5-obs2-loading | passed; engine-drawn reference; `ref check` of sw2's loading-model against it: pass |
| menu-lockbook, observe | s7-menus5-obs3-lockbook | hang at the book (frame tap error above); lockpicking observed |
| menu-lockpick, owned | s7-menus5-sw3-lockpick | passed; `ref check` against obs3's lockpick shot: pass |

Final runs (`s7-menus5-fin-*`, on c9ec20b6d's code), all passed: guest+quick, native-vulkan+quick, native-d3d12-quick,
native-vulkan+menu-scenes, guest+menu-scenes, native-vulkan+menu-main (main menu 1.0 per frame), native-vulkan+menu-main-load,
native-vulkan+menu-items, native-vulkan+facegen, native-vulkan+cadence-drop-cells, native-vulkan and guest +menu-loading,
native-vulkan and guest +menu-lockpick.

Shot references to accept: `s7-menus5-fin-nvk-loading` (loading-model, world; checked against the engine-drawn `-obs2-loading`),
`s7-menus5-fin-nvk-lockpick` (lockpick; checked against the engine-drawn `-obs3-lockbook`), and the guest runs of both configs.
