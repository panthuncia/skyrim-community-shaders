# snap5-localmap: the local map's look, and what its CS port needs (roadmap phase 5, "Menu scenes")

Lead `localmap`, branch `snap5/localmap` (base `org_integration` 0c0e67803), stage `s7-localmap`. Starts from
[snap5-map.md](snap5-map.md) (open items 1-3), decisions 146-150, 202, 207-210.

## State

| Item | State |
|---|---|
| 1. The look in graph mode (white land, opaque panel) | done: root cause found and fixed (guest and native); shots match plain D3D11 |
| 2. Port the local map's draws to CS | not done: RE and a census of every pass kind the batches draw (below); the port is the next item |
| Prebuild Utility 0x10000003 / 0x200001a | not needed as code: the lab's manifest lists them now (below); the port should queue its own keys at setup |

## 1. The look

### What the local map writes (measured)

Three debug runs replaced ISLocalMap's output with kLOCAL_MAP_SWAP itself (stripes of its colour and of its alpha as grey), in plain
D3D11, guest and native (`s7-localmap-d1-*-swap`), and a fourth set put the land draws' light inputs in stripes (`-d2-*-light`).

- **Alpha is the fog of war's only.** Objects and land draw with write mode 1 (mask RGB). LocalMapRenderer::Render sets the clear colour
  (0, 0, 0, 1) (AE 0x140e44bf0 with 1.0) before `SetRenderTarget(0, kLOCAL_MAP_SWAP, clear)`, and the fog pass writes alpha only
  (write mode 8, depth off, `(0, 0, 0, vertex alpha)`). ISLocalMap's alpha is `(edges + luminance) * (1 - w)`: explored ground (w = 0)
  shows, the rest is clear. The alpha stripes were the same in all three modes.
- **The land's colour was the difference.** Plain D3D11's land stripes show lit terrain in moderate tones; guest's and native's were
  white. The draws read the same rows in both: sun colour (10, 9.7, 9.5) at noon (logged at the offscreen loop and at DrawInterface:
  the same value, so moving MapMenu::PreDisplay changed nothing), direction (0, 0.062, 0.998). The settings stripe showed
  `PBRLightingCompensation` = 1 in D3D11 and pi in graph mode: D3D11 compiles the land with ENABLE_LL (Linear Lighting), the menu
  scenes' ORG Lighting build `#undef`s it. Without Linear Lighting the PBR land (pixel descriptor 0x8000029: VC, TRUE_PBR,
  LANDSCAPE_SEAMS) is lit at 10 x pi with gamma-encoded albedo, and saturates; with it the linear output (pre-exposed) lands in the
  8-bit swap as moderate tones, which ISLocalMap turns into the dark map with bright edges players know.
- **Native's opaque panel** was dropped draws, not a native-only bug: the first local map frames refused programs that were not
  prebuilt and textures still streaming, and native drops the rest of a refused batch (guest's engine draws it). With the lab's manifest
  warm (`-d1-native-vulkan-swap`) native's swap equals guest's.

### The fix (6b121ace5)

A Lighting program variant, `kLightingLinearVariant` (`LIGHTING_ORG_LINEAR`, MenuScenes.h / MenuScenesDraws.cpp), keeps ENABLE_LL in
Lighting.hlsl's ORG build; the local map's land draws take it (`TryLatchLighting`). With Linear Lighting off the variant compiles the
same as before. Menu models keep the variant without Linear Lighting (out of this item; see open items).

`s7-localmap-f1-guest-local` against `s7-map-a7-d3d11-local`: the same look (dark land, bright object edges, explored area only); `ref
check` passes (it passed before the fix too: the check is too coarse to see this).

## 2. The port: what LocalMapRenderer::Render does (AE 16335 at 0x140242e00, SE 16094)

In order:
1. `State.shadowSceneNode[0]`'s byte +0x219 = 1 (local map mode), the renderer's accumulator +0x148 = that node; the node's child 3
   is shown (flag 0xf4 bit 0 cleared) for the render; globals 0x14332b210 = 1, 0x142032dfc/dfd = 0; `BSTreeManager` mode 1
   (FUN_140438580).
2. Clear colour (0, 0, 0, 1) (FUN_140e44bf0).
3. Culling with its BSCullingProcess (+0x301f8 job, camera +0x302b8 = LocalMapCamera's NiCamera, ortho):
   - no interior cell (TES +0xc0 null): the loaded cells (FUN_140243b00 over TES +0x78's grid) or the sky cell; else the interior cell.
     Per cell (FUN_140243ea0): the cell node's children {2, 3, 6, 7} (0x14178d0b0), child 2 (land) only when the worldspace has no
     flag 0x40 at +0xa0; each culled with a temporary culling process in cull mode 3 into the local map's accumulator.
   - FUN_140649f00(Main)'s +0x10 node's +0x58, then shadowSceneNode[0]'s children 9 and 8 (the census names them ObjectLODRoot's).
4. `SetCurrentDepthStencilTarget(kMAIN, clear)`, `SetRenderTarget(0, kLOCAL_MAP_SWAP, clear)`, SubmitAccumulator (the land and objects
   batch, hooked at +0x342).
5. Fog of war (global 0x141fd6f46): `RenderFirstPersonView_End`, render mode 0x13, cull the fog root (+0x30358), write mode 8, depth
   mode 0, `SetRenderTarget(0, kLOCAL_MAP_SWAP, restore)`, SubmitAccumulator (+0x433), then depth 3 and write mode 1 back.
6. `SetCurrentDepthStencilTarget(none)`, ISLocalMap's param row 0 = (1 / 0x14328c05c, 1 / 0x14328c060, 0, 0), then
   `RenderEffect(0x62, kLOCAL_MAP_SWAP -> kLOCAL_MAP)`; everything set in 1 is put back.

### Census of the batches' passes (guest, `s7-localmap-c1/c2-guest-census`, Whiterun at noon)

| Pass | Descriptor (vertex / pixel) | States | Example |
|---|---|---|---|
| object (Lighting property -> Utility RENDER_BASE_TEXTURE) | 0x200001a / 0x2000002, +Vc 0x200001b, +AlphaTest 0x200009a/0x200009b, +TreeAnim 0x600009b, +TruePbr pixel 0x42000003 | write 1, depth 3, cull by two-sided, alpha test from the property | WhiterunBridge, MountainPeak01, Ivy08_Animated |
| land (Lighting) | 0x8000001 / 0x8000029 (MTLand), 0x13000001 / 0x13000029 (MTLandLODBlend), +0x10000 | write 1, depth 3 | Block (2, 0) |
| water (Water, local map technique) | 0x4802 / 0x4802 | write 1, depth 1, stencil mode 30 | DefaultProceduralWater |
| tree LOD (DistantTree) | (no ORG program) | | |
| fog of war (Lighting property -> Utility LOCALMAP_FOGOFWAR) | 0x10000003 / 0x10000003 | write 8, depth 0, cull 0; samples t0 (a 4096 BC7) | FogOfWarOverlay |

About 1380 passes per local map frame (more than `kMaxDraws` = 2048 can hold in a busier area). Land rows: sun (10, 9.7, 9.5), the
directional ambient rows, the fog rows (FogParam (0.053, 0, 0.65, 1)); FrameBuffer c42 (1, 1, 0, 0). Objects: TexcoordOffset (0, 0, 1, 1),
t0 the diffuse.

### Plan for the port (decision 149: observe, then switch)

- A native detour body for LocalMapRenderer::Render (MenuViewsFeature.cpp's `LocalMapRender`), shaped as `Scenes::Native::Render`:
  `Skeleton::ClearDepth(kMAIN)`, `Skeleton::ClearColour(kLOCAL_MAP_SWAP, {0, 0, 0, 1})`, a `View::LocalMap` run, a `View::LocalMapFog`
  run, and ISLocalMap staged with its row through `NativePost`'s `StageMenuKey` (as `RunMenuHdr` does), with no RenderEffect call.
- Visibility: the roots in 3 walked by CS with the ortho camera's planes, leaves tested with ORGModuleServices' sphere cull (DCLF's SIMD
  kernel); hidden and app-culled flags as the engine's cull mode 3; no BSCullingProcess.
- Draws: objects through `ComputeUtility` with a local map `UtilityView` (RENDER_BASE_TEXTURE = 0x2000000 on
  `ShadowUtilityTechnique`'s bits, write 1, depth 3; t0 the diffuse); the fog with LOCALMAP_FOGOFWAR (write 8, depth 0); land through
  `ComputeLighting` with local-map inputs (the scene node's sun and ambient; no menu lights) and `kLightingLinearVariant`; water's local
  map technique (unported: the water port returns `localMap`); tree LOD needs an ORG program for DistantTree.hlsl (DCLF's TreeLod has
  pulled programs for the world, not this family).
- Prebuild: the port knows every key it draws with (the census list), so it queues them at setup instead of relying on the manifest.
- Observe first: compare each computed draw with the latched one (MenuScenesCapture's latch is the reference), then own and delete the
  observe code and the local map's latch path.

## Detector

Not changed by this item: local map frames still read about 35 engine render calls (the port removes them).

## Runs (Dev-Fast)

| Config | Label | Result |
|---|---|---|
| d3d11+map-local, features off | s7-localmap-x1/-x2/-x3-d3d11-nofeat | failed or timed out: the `weather` step needs editor ids, and with every feature off the run crawls (5 fps) |
| d3d11+map-local, Physical Sky, height fog, View Atmosphere off | s7-localmap-x4-d3d11-nosky | passed; land still dark |
| d3d11+map-local, Linear Lighting off | s7-localmap-x5-d3d11-noll | passed; land still dark (see open items) |
| d3d11+map-local, 17 world features off | s7-localmap-x6-d3d11-orgset | passed; land still dark |
| guest+map-local, census | s7-localmap-c1/-c2-guest-census | passed; census above |
| d3d11+map-local, RenderDoc capture | s7-localmap-r1-d3d11-rdc | passed, but no capture file was written in the lab slot |
| d3d11, guest, native-vulkan +map-local, swap debug | s7-localmap-d1-*-swap | passed; swap colour and alpha per mode |
| guest+map-local, sun logged | s7-localmap-c3-guest-sun | passed; sun 10 at both call points |
| d3d11, guest +map-local, light debug | s7-localmap-d2-*-light | passed; ENABLE_LL found |
| guest+map-local, the fix | s7-localmap-f1-guest-local | passed; matches D3D11 |

Final runs: see the report (`s7-localmap-fin-*`).

## Decided by the agent, to confirm

- **Plain D3D11's look is the target** (as the prompt says), although it comes from Linear Lighting's linear output written to an
  8-bit target; vanilla's local map (no Linear Lighting) is the bright one graph mode drew.
- **Only the local map's land takes the Linear Lighting variant.** Menu models in plain D3D11 also draw with ENABLE_LL (and RenderToUI);
  changing the menu scenes' build is outside this item and would move every menu scene reference.
- **No static prebuild list** for the local map's Utility programs: the manifest already holds them after one session; the port will
  queue the keys it computes.

## Proposed decision-log entries

- The local map's graph-mode look (white land, called "white tiles") was Linear Lighting: plain D3D11 compiles the land's PBR Lighting
  with ENABLE_LL and the swap target stores that output; the menu scenes' ORG build left it out, so the sun (10 at noon) saturated the
  land. The local map's land draws take a Lighting variant that keeps ENABLE_LL (`LIGHTING_ORG_LINEAR`). The alpha (fog of war only)
  was right in every mode; native's opaque panel was refused batches with cold programs, not a native bug.

## Open items

1. **The port** (plan above): observe, then own; removes the ~35 engine render calls per local map frame.
2. `s7-localmap-x5` (D3D11, Linear Lighting disabled) still drew the land dark, which the ENABLE_LL finding does not explain
   (with the feature off, ENABLE_LL is not defined). Check whether a disabled feature still contributes its define through the shader
   store, before relying on the Linear-Lighting-off case.
3. Menu models' ORG Lighting build without ENABLE_LL vs plain D3D11 with it: compare the menu scenes with D3D11 shots.
4. `kMaxDraws` (2048) per batch: a dense local map can pass it (1380 passes here).
5. RenderDoc captures in a lab slot were not written; the recipe needs the MO2 instance or a fixed path.
