# snap5-localmap2: the local map's draws are CS's (roadmap phase 5, "Menu scenes")

Lead `localmap2`, branch `snap5/localmap2` (base `org_integration` 6ee315a9a), stage `s7-localmap2`. Starts from
[snap5-localmap.md](snap5-localmap.md) (its RE and pass census) and decisions 149, 207-210 and 221.

## State

| Item | State |
|---|---|
| Native LocalMapRenderer::Render | done: native draws the local map from the scene; the engine's function is not entered |
| Visibility (no engine culls) | done, with an open difference: 174 extra and 2 missing of ~1,380 passes (below) |
| Objects (Utility RENDER_BASE_TEXTURE) | done: observe 0 differences (frame row c21 by 1 ulp, decision 148) |
| Land (Lighting MTLand / MTLandLODBlend, Linear Lighting variant) | done: observe 0 differences (same ulp) |
| Water (technique LOD, stencil mode 30) | done: a new ORG build, `WATER_ORG_SCENE`; observe 0 differences but ReflectionColor.w (the timer; explained below) |
| Fog of war (Utility 0x10000003, write 8, depth off) | done: observe 0 differences (its t0 is a stale binding nothing reads) |
| Tree LOD | not drawn by the local map: no pass seen in guest's census or native's observe; a tree LOD pass is fatal (to confirm) |
| ISLocalMap | staged natively with its row (1 / width, 1 / height) through the menu composite's step |
| Native staging (clears) | `Skeleton::ClearDepth(kMAIN)`, `Skeleton::ClearColour(kLOCAL_MAP_SWAP, {0, 0, 0, 1})` |
| Program keys queued at setup | done: 47 keys (objects, fog, land, water); deferred to the first native menu scene render when the engine's shaders are not made yet |
| More than 2048 passes | the local map's batch takes 8192 draws and an 8 MiB latch (`MaxDrawsOf`, `LatchBytesOf`); past that is fatal |
| Observe scaffolding | deleted at the switch (it lives in git history: commit 17fb72738, `MenuScenesLocalMapObserve.cpp`) |

Guest keeps the engine's render with its latched batches (out of scope); only its draw limit grew with the shared `MaxDrawsOf`.

## What was built

- **`MenuScenesLocalMap.cpp`** (new): `CollectLocalMap` (camera +0x302b8, fog root +0x30358, the fog global AE 0x141fd6f48 / SE id
  501260; the loaded grid cells' attached nodes' children {2 with land, 3, 6, 7}, or the interior cell, plus the attached sky cell; the
  first list process's portal graph list (+0x30190, +0x10, +0x58); ShadowSceneNode[0]'s children 9 and 8), `LocalMapVisible`,
  `LocalMapPassOf`, `SampleLocalMapInputs`, `SetLocalMapTreeDetail` (FUN_140438580(BSTreeManager, 1) ported), `SampleLocalMapWater`
  and `ComputeWater`.
- **Visibility.** Two cull modes, both observed:
  - a cell's node children are frustum-culled (cull mode 3) from their children down, a level at a time with ORGModuleServices'
    SIMD sphere kernel (`org::cpu::WideFrustum`, SSE fallback). The roots themselves are hidden in native (DCLF's hidden nodes) and
    keep a stale (0, 0, 0, r 1) bound, so the root is never tested, as the engine shows them for the cull (FUN_140243ea0);
  - every other root (the portal list, ShadowSceneNode[0]'s children, the fog root) takes the local map process's own mode, which keeps
    everything outside hidden subtrees: the engine draws all 25 fog tiles (13 outside the frustum) and ObjectLODRoot's far contents
    (Markarth's docks in Whiterun's map).
  - Switch nodes give their active child, billboards turn to the camera. BSFadeNode does not update fades in the local map (its
    OnVisible skips while the render's 0x142032dfd is 0), so the stale fade decides GetRenderPasses_LocalMap's fade test, as the engine's.
- **Passes** (`ComputeUtility` views `LocalMap`/`LocalMapFog`): GetRenderPasses_LocalMap (vtable 0x2C, AE 0x1414afb00, SE 0x1412c7cd0):
  mode 0x12: none for skinned or refracting, blended, or fade x material alpha below 1 (SE 0x1415232d8 = 1.0); technique
  `UtilityShaderDecl | 0x80 with alpha test | 0x2000000`; mode 0x13: `UtilityShaderDecl | 0x10000000`. Landscape (flag 14) takes its own
  Lighting passes. States: write 1 (objects) / 8 with depth and cull off (fog), stencil off. AlphaTestRef only where the pixel shader
  has PerGeometry (not the fog).
- **Land** (`ComputeLighting` with `Inputs::localMap`): the sun only, no previous camera (c16-c19 and PreviousWorld from a zero eye),
  EyePosition = -eye (the local map accumulator's eye is zero), RenderToUI 0, True PBR's glint bit from `HasGlint`, Landscape Seams'
  bit from `IsBlended`, t6 and t14 the default height map (3, 3), the Linear Lighting variant.
- **Water**: `Water.hlsl`'s `WATER_ORG_SCENE` (the water family's latch struct, the menu scenes' push block, the local map camera's
  FrameBuffer rows, the world features off; compiles offline for SPIR-V with and without VC); a `menu scenes (Water)` program family;
  descriptor 0x4802 (+1 with vertex colours; observed: never Reflections or Cubemap whatever flags 0x40/0x4000); the water port's
  Evaluate* and Assemble with local map frame inputs; TextureProj zero (observed; LOD reads none); depth 1, stencil 30 ref 1, cull 0;
  t4 the first normal map; drawn after the land and objects (the post-resolve group).
- **Native body** (`Native::RenderLocalMap`, `MenuViewsFeature.cpp`'s detour): clears, the two runs, ISLocalMap
  (`ImageSpace::LocalMapEffect` -> `NativePost::RunLocalMap`, in an effect scope like RenderEffect's). It runs only while the UI stage
  holds its clients (a frame that closes the map draws nothing of it). A draw left out (building program, streaming texture, no buffers)
  skips that render; any other refusal is fatal (decision 149).

## Observe (native Vulkan, `s7-localmap2-obs1..7`)

Per draw, against the latched batch (rows with written masks, descriptors, permutation block, states, textures, frame rows):
Utility 285,633 draws, Lighting 4,015, fog 7,350, water 332: 0 differences except the explained ones:
- FrameBuffer c21 (the inverse projection) by 1 ulp (decision 148);
- water ReflectionColor.w: the engine's live timer global (AE 390953) vs the adopted snapshot's (the render half reads captured values,
  decision 211); it only scrolls the normals;
- the fog of war's t0: a stale view the fog pass never reads (its colour is (0, 0, 0, vertex alpha)).

Membership, not explained: CS draws 174 passes the engine does not and misses 2 (an NPC's weapon). The extras sit at the frustum's
far plane (107, under the land) and its edges (67); their spheres overlap the frustum by up to ~1,250 units while matched passes
overlap by as little as 1, so the engine's rule is not a sphere test with other planes. A multibound box test was tried
(`-obs7`) and made it worse (land blocks went missing): reverted.

## Detector (`[EngineZones]`, native Vulkan; Dev-Fast)

| Frames | Before | After |
|---|---|---|
| local map (frozen world) | 35.0 per frame, max 43 (`s7-localmap-fin-nvk-local`) | 2.0 per frame, max 10 (`s7-localmap2-sw4-nvk-local`) |
| world | 1.0 | 1.0 |

The two calls left in a local map frame are Main::DrawFrame and RenderEffect inside AE 107130 (0x1414cba80, called from
Main::DrawFrame in every frozen-world frame): the frozen world's menu background copy (effect 0x22, kMENUBG into the framebuffer, the
menu composite's FrozenKey step). It is no part of LocalMapRenderer::Render: every frozen-world menu has it (open item 4). The local
map's own calls (its batches and its RenderEffect) are gone.

## Runs (Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-vulkan+map-local, observe | s7-localmap2-obs1 .. -obs4, -obs6, -obs7 | passed; differences above, fixed or explained |
| native-vulkan+map-local, observe | s7-localmap2-obs5 | timeout at the main menu: menus' fatal "no shader for vs 0x1" on a cold D3D11 shader (fixed, below) |
| native-vulkan+map-local, switched | s7-localmap2-sw1, -sw2 | hang: ISLocalMap staged outside an effect scope ran in the next world frame (fixed); the map drew |
| native-vulkan+map-local, switched | s7-localmap2-sw3 | timeout at the main menu: the same cold-shader fatal (fixed) |
| native-vulkan+map-local, switched (detector) | s7-localmap2-sw4-nvk-local | passed; frozen world 2.0 per frame, world 1.0 |
| guest+map-local | s7-localmap2-fin-guest-local | passed |
| d3d11+map-local | s7-localmap2-fin-d3d11-local | passed (fresh reference) |
| native-vulkan+map | s7-localmap2-fin-nvk-map | passed |
| native-vulkan+quick | s7-localmap2-fin-nvk-quick | passed |

Skipped under the test scope rule: native-d3d12-quick (no ORG, BasicRHI or native device change), guest+quick (guest's only change is
the local map batch's larger draw limit, which guest+map-local covers), native-vulkan+cadence-drop-cells (nothing stepped by game delta).

Shots: native (`-sw4`) and guest (`-fin-guest-local`) local maps match each other and the previous native and guest references. Both
differ from plain D3D11 (old `s7-map-a7-d3d11-local` and fresh `-fin-d3d11-local`) in the explored area at the panel's upper left
(D3D11 shows land there that both graph modes leave dark): that difference predates this item (it is in snap5-localmap's native and
guest shots too) and is open item 5.

Found on the way: native menu scenes made a draw whose D3D11 shader CS's cache has not compiled yet fatal ("no shader for vs 0x1 ps
0x1", the main menu logo on a cold cache, 2 of 13 runs). It is left out of that render now, as a building program is (decided by the
agent, to confirm).

## Decided by the agent, to confirm

- **Tree LOD is fatal, not ported**: no DistantTree pass was seen in any local map batch (guest census, native observe); tree LOD hangs
  under the terrain manager's root, which the local map does not cull.
- **The water's timer is the snapshot's**, not the engine's live global.
- **The 174 extra / 2 missing passes are accepted for now** (open item 1): they lie under the land or at the panel's edges.
- **Guest's local map batch takes the larger limits too** (shared `MaxDrawsOf`): fewer refusals in a big city, otherwise the same.
- **A native menu scene draw whose D3D11 shader is not compiled yet is left out** of that render (was fatal).

## Proposed decision-log entries

- The local map is CS's in native: `RenderLocalMap` computes its land and objects (Utility base texture, landscape Lighting with the
  Linear Lighting variant), water (a new `WATER_ORG_SCENE` build) and fog of war from the scene with CS's visibility (a cell's children
  by DCLF's SIMD frustum cull, other roots in the process's all-pass mode), clears its targets through the skeleton and stages
  ISLocalMap natively. Observe: 0 differences per draw; membership 174 extra / 2 missing open. Tree LOD never appears in its batches.

## Open items

1. Membership: find the engine's rule that drops the 174 edge and below-ground passes (and keeps the NPC weapon).
2. SE: the AE ids are checked; the SE pairs come from the Address Library and SE decompiles (fog global, size globals, water shader).
3. Program keys outside the queued set (other Utility declarations) still leave their draw out for a render while they build.
4. The frozen world's menu background copy (AE 107130's RenderEffect in Main::DrawFrame) stays the engine's call in every frozen-world
   frame; staging its FrozenKey step natively makes local map frames read 1.
5. The explored area's look against plain D3D11 (both graph modes, pre-existing): compare the fog of war overlay's vertex alpha and the
   land in that area between D3D11 and graph mode.
