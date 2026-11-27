# snap3-skeleton: CS drives the native frame skeleton (phase 3, decision 70)

Lead: `skeleton`, branch `snap3/skeleton` (base `org_integration` c224bd46f), stage `s7-skeleton`. Phase 3 of
[render-thread-snapshots.md](render-thread-snapshots.md) ("done when the engine-render detector shows no world-frame engine render
call in native"); decisions 8, 57, 64, 70 and 122-186 of [render-thread-decisions.md](render-thread-decisions.md); the skeleton
inventory in `snap3-facegen.md` section 2 is the map.

## Result

Native world frames make **one** engine render call: Main::DrawFrame itself (CS's hook). DrawInterface counts as a producer, as
before. Main::RenderScene and Main::RenderPlayerView are no longer entered in native.

| Config | Before (last lead's number) | After |
|---|---|---|
| native-vulkan+quick | 9.0 (s7-menus4) | 1.0 |
| native-d3d12-quick | - | 1.0 |
| native-interior | 12.1-12.9 (snap3-fpv) | 1.0 |
| native-tour | 16.9 (snap3-water, before later merges) | 1.0 |
| first-person | 16.6 (snap3-fpv) | 1.0 |
| post-effects | 13.9 (snap3-fpv) | 1.0 |
| shadows-blend | 14.5 (snap3-fpv) | 1.0 |
| the rest | see the runs table | 1.0 |

Guest is unchanged (67 calls a world frame in guest+quick; the RenderScene #2 row is gone from the table, below).

## 1. The driver (`src/RenderGraph/Engine/Native/RenderScene.{h,cpp}`)

A call-site thunk on Main::DrawFrame's world call of Main::RenderScene (SE 35556 +0x2DC, AE 36555 +0x4C8; byte-checked, fatal on a
mismatch; installed only when native is requested, from `Snapshot::InstallHooks`). In native it runs `RenderScene`, which follows
RenderScene's and RenderPlayerView's calls in their order:

- **CS's points** are called through the call sites' hook chains (`EngineZones::CallTarget`, decision 57): +0xD3 SetupSunAndSky (DCLF's
  frame start and the water render point), +0x1C4 the shadow casters, +0x2EC the shadow maps, +0x395 RenderDepth, +0x39C the masks,
  +0x3A1 the precipitation, +0x841 RenderWorld, and in the first-person block +0x8CE ClearMasks (DCLF's sun mask hook and the
  first-person hook, which skips the engine) and +0x954 RenderFirstPersonView; then the compute SAO when its flag is set, RenderWorld
  PostProcess (the post chain's detour runs CS's native body) and the screen splatter. Calling the chains keeps every feature's wrapper
  (Terrain Blending around RenderDepth, Upscaling and Skylighting around the precipitation, Deferred's pre- and post-work around the
  shadow maps and RenderWorld) in its place without a second copy of it. A point whose call still leads to the engine's own function
  (a feature that did not hook it) is logged once ("a point of Main::RenderPlayerView is not hooked"); no run logged one.
- **Game-side work kept** (decision 8), each where the engine made it:
  - the reflections' camera (SE 528353 / AE 415299) set to the world camera: the radial blur modifier reads it on the game side
    (snap3-post's open item);
  - the branch reads: the main menu test (AE 82074), the first-person root's hidden bit, the camera's free state, the movement
    controller (AE 37792 and its vfunc +0x78, RenderDepth's and the first-person view's second argument), UI +0x160;
  - the list processes' camera and SetFrustum (SE 69699 / AE 71081): ShadowSceneNode::ProcessAllQueuedLights (SE 99703 / AE 106337)
    tests each light's visibility and fade against the first process's planes and camera (FUN_141508eb0), and DCLF's AfterFullFrustum
    point reads its camera. Kept for every process: DCLF's primary cut keeps all the process pointers (decided by the agent, to confirm:
    the others' frustums may have no reader);
  - ProcessAllQueuedLights itself, with the first-person root shown around it, as RenderPlayerView does (game-visible light lists,
    StealthLights);
  - the first-person root's hidden bit set after SetupSunAndSky, and the sky result's node hidden around the casters in first person;
  - the first-person root shift (SE 39494 / AE 40573, the player's nodes and magic light) with AE 107123 before the shadow maps and
    with AE 107124 after the first-person view: DCLF's first-person camera (415007) and the depth of field's near and far read what
    107123 builds;
  - the sky colours copied into the lighting shader (415094 +0xD0/+0xE0, DCLF's material port reads them), the SAO helper
    (ImageSpaceManager +0x1C8 +0x58/+0x64, native SAO's sparkle colour) and the snow helper (+0x1E8 +0x0C/+0x18);
  - the first-person camera's port when 411491 is set;
  - Main::Draw's tail (SE 100425 / AE 107143), one engine call: the sun's occlusion query flag (Sun +0x4C), the release of an
    accumulator's pass list (415013), of a frozen ImageSpace texture and of the batch renderer's last state, which the engine-drawn menu
    scenes and the frozen background still fill (the fade enable count in it is S5's patched branch).
- **The clears before RenderWorld** go through CS's skeleton as Renderer::ClearColor's detour takes them (`Skeleton::ClearColour`): the
  normals target (`WorldPass::NormalsTarget`) to (c, c, 0, 0) with c from SE 230690 / AE 187405, kMAIN to RenderScene's clear colour
  (SE 527696 / AE 414618, alpha 1), the snow target (0x70) to zero with improved snow (391105). None was refused in any run.
- **Not made** (render state that only fed the engine's own render functions; the observe run below showed no reader in native):
  the TLS task tag and the 411306 flag around RenderScene, the clear colour and viewport calls, BSShaderManager::State's eye (+0x2F4)
  and the first-person accumulator's eye (415011 +0x16C), AE 107122 (the first-person camera at DrawWorld's eye: 107123 sets it in the
  frames that read it), the State camera cache calls (AE 77506 per camera and jitter flag, AE 77505 rotating the previous matrices, SetCameraData #1 and #3, AE 77503), the
  list processes' resets (AE 76557: nothing fills them, the list culls do not run and the light walk only reads the planes), the map
  menu flag (414950: its readers are the engine's RenderDepth and the LOD shift window functions, which native does not enter), AE
  107018 (empty), AE 107131 and the other target binds, the dirty bits, the posAdjust copy (411319), the refraction group's count and
  clears (AE 106598/106600: nothing registers into the accumulator in native, AE 107145 never ran), PlayerCamera::GetFirstPersonNode
  Position (its result was unused), the job stage signals 6 and 7 (S1, already CS's), AE 39034 (S4, already CS's).

One reader of the dropped State camera cache was found by the first observe run: the SSR ray march (`NativeWorld.inl`
`ReadReflections`) looked up the world camera's cache entry (AE 77507) and failed fatally once nothing made it. Native now takes the
snapshot camera's view (`MainCamera().view`, which `FrameCamera::Capture` builds as SetCameraData does); guest keeps the lookup.

`EngineZones.cpp`: the row "Main::RenderScene #2 (in Main::DrawFrame)" is removed, as RenderWorld's and RenderPostProcess's were
(decision 136): the call goes to CS's driver in native, and the sites inside RenderScene and RenderPlayerView count the engine's
skeleton wherever it still runs (guest, the frozen menu branch's RenderScene #1).

## 2. Observe (scaffolding removed)

World frames alternated between the engine's RenderScene and CS's driver. Probes at every point (the call sites above, the post
chain's call and RenderScene's return) took 8-byte fingerprints of BSGraphics::State, its first four camera cache entries, the shadow
state (0x400 bytes), BSShaderManager::State, the world and first-person cameras, three accumulators, the clear colour, the lighting
shader's colours, the SAO and snow helpers, the first two list processes and the globals above. A driver frame was compared with the
engine frame before it; chunks that already differed at the first point (camera motion, frame counters) were taken as the baseline.

- `s7-skeleton-a-observe-fp`: fatal in the SSR ray march (above). Fixed.
- `-a-observe-fp2`, `-fp3` (first-person config): third person only (see the open item on first person). `-a-observe-load`
  (menu-main-load, 988 first-person world frames, every point visited): the only differences beyond the baseline are in the shadow
  state at RenderWorld, ClearMasks, the first-person view and the post chain: RT0-2, the depth target and their modes (the target
  binds), and the viewport (+0x78; the engine leaves 1280x720, the driver the full size). Every reader of those inside RenderWorld uses
  `WorldPass::Current()` (sky, water, precipitation, families' `LatchViewport`); the post chain's steps set their own; the blood
  splatter takes the back buffer's size. Nothing else differed, including the cameras, the lighting shader and helpers, the processes
  and the globals.

## 3. Runs (stage `s7-skeleton`, Dev-Fast, `env.CS_SNAPSHOT_DETECT=1`)

| Config | Label | Result | Render calls a world frame |
|---|---|---|---|
| native-vulkan+first-person, observe | `s7-skeleton-a-observe-fp` | fatal in the SSR ray march (fixed) | - |
| native-vulkan+first-person (tai, timescale 0, tm), observe | `-a-observe-fp2` | passed; third person only | - |
| native-vulkan+first-person, observe | `-a-observe-fp3`, `-fp4` | passed; third person only (fp4 logs the branch reads) | - |
| native-vulkan+first-person, engine-only build | `-a-engineonly-fp` | passed; the first-person block never runs after the pov switch (open item) | - |
| native-vulkan+menu-main-load, observe | `-a-observe-load` | passed; 988 first-person frames, differences above | - |
| guest+quick | `-b-guest-quick` | passed | 67 (guest) |
| native-vulkan+quick | `-b-native-vulkan-quick` | passed, shot reference passed | 1.0 |
| native-d3d12-quick | `-b-native-d3d12-quick` | passed | 1.0 |
| native-vulkan+native-interior | `-b-native-vulkan-native-interior` | passed | 1.0 |
| native-vulkan+native-tour | `-b-native-vulkan-native-tour` | passed | 1.0 |
| native-vulkan+first-person | `-b-native-vulkan-first-person` | passed | 1.0 |
| native-vulkan+menu-main-load (first person from a load) | `-b-native-vulkan-menu-main-load` | passed | 1.0 |
| native-vulkan+post-effects | `-b-native-vulkan-post-effects` | passed | 1.0 |
| native-vulkan+shadows-blend | `-b-native-vulkan-shadows-blend` | passed | 1.0 |
| native-vulkan+native-river | `-b-native-vulkan-native-river` | passed | 1.0 |
| native-vulkan+rain | `-b-native-vulkan-rain` | passed | 1.0 |
| native-vulkan+screen-blood | `-b-native-vulkan-screen-blood` | passed; blood drawn | 1.0 |
| native-vulkan+upscalers | `-b-native-vulkan-upscalers` | passed | 1.0 |
| native-vulkan+window-resize-stress-taa | `-b-native-vulkan-window-resize-stress-taa` | passed (12 resizes) | 1.0 |
| native-vulkan+menu-scenes | `-b-native-vulkan-menu-scenes` | passed | 1.0 |
| native-vulkan+facegen | `-b-native-vulkan-facegen` | passed | 1.0 |
| native-vulkan+cadence-drop-cells | `-b-native-vulkan-cadence-drop-cells` | passed, all three shots | 3.0: Main::DrawFrame's site counts every game update while only one in three renders (drop:3); nothing else |

Live engine reads per world frame are unchanged within scene noise (quick 642, interior 510, tour 523): the engine skeleton's own
reads were never counted, and the driver's game-side reads use CommonLib directly, as the skeleton did.

## 4. What remains in a native world frame

- Render calls: Main::DrawFrame (CS's hook) only.
- Producer: MenuManager::DrawInterface (decision 64), UI3DSceneManager::Render for owned menu schemes.
- Helpers in Main::DrawFrame (it still runs): Renderer::Lock, Begin and End (Begin hosts CS's frame start, the snapshot adoption and
  the window resize; End presents through CS's Present hook; Lock pairs the main thread's frame with the loading loop's: all three go
  with phase 7's render thread, snap7-prep 1, 2 and 6), AE 82085 (menus' PreDisplay, UI game logic), AE 107698 / 107646 / 107502 (the
  engine shaders' per-frame cache, a shader state word and the render object pools, which the engine-drawn menu scenes still use),
  AE 52210 / 52211 (MenuControls keys, game input), FreezeTest #1/#2, FrameJobs and WaitForJobTask (game state and the UI job wait), AE
  51639 (copies the world camera's world-to-camera matrix and port into the globals 0x1431ad3a0 / 0x1431ae960 that the game's
  WorldPtToScreenPt3 callers use: game side), AE 36859 (the screenshot request).
- Engine calls the driver makes itself (not counted, decision 57): 107123/107124 and the player shift (first person), 106337, the
  processes' SetFrustum, 107143's tail, RenderSaoCs with bSAO_CS_Enable (its dispatches are CS's steps; the engine body still runs).
- Engine state the driver writes: 415299, the processes' camera, the first-person root's hidden bit, the sky colours, the first-person
  camera's port.

## Decided by the agent, to confirm

- SK-a. CS's points are called through the call sites' hook chains (decision 57's mechanism) rather than by name, so every feature's
  wrapper keeps running in its place; a point not hooked by CS is logged, and the engine's function then runs.
- SK-b. The game-side work stays in the engine's order inside the driver (single-threaded frame); phase 7 moves the reads and the game
  half into the capture and the publish.
- SK-c. The first-person root shift and its camera (107123/107124) are kept as engine CPU calls: DCLF's first-person camera reads them.
  Porting them means DCLF building the first-person camera from the snapshot.
- SK-d. Every list process keeps its camera and frustum (only the first has a proven reader).
- SK-e. Main::Draw's tail (107143) is kept whole: its releases serve engine paths phase 5 still runs.
- SK-f. The detector row for DrawFrame's RenderScene #2 call is removed (decision 136's precedent).
- SK-g. The shadow state's target binds and viewport that RenderPlayerView left are not written (no native reader; observe above).

## Proposed decision-log entries

- **CS drives the native world frame skeleton** (decision 70 built): Main::DrawFrame's world call of Main::RenderScene goes to CS's
  driver; RenderScene and RenderPlayerView are not entered. CS's points are called through their call sites' hook chains; the
  game-side work (the reflections' camera, the queued lights with the first process's camera and frustum, the first-person root bits
  and shift, the sky colours, Main::Draw's tail) runs in the engine's order; the clears before RenderWorld go through the skeleton.
  Native world frames: 1 engine render call (Main::DrawFrame), from 9-17.
- **Native SSR takes the snapshot camera's view**: the State camera cache is no longer filled in native world frames.
- **Phase 3's detector goal is met**; what remains in DrawFrame (Lock, Begin, End, the UI and input helpers) goes with phase 7.

## Open items

- **First person after a pov switch never runs the first-person block in native**, before this branch too (checked with an engine-only
  build, `s7-skeleton-a-engineonly-fp`): RenderPlayerView leaves the first-person root hidden each frame, and only the block itself
  (or the engine's ClearMasks, skipped since snap3-fpv) shows it again, so after a switch from third person the engine's first-person
  test stays false. First person is still drawn (DCLF's Z-prepass draws it); a load into first person runs the block every frame
  (menu-main-load). The block's DCLF colour point and 107123's camera therefore depend on how the session started. Needs its own look.
- `perf-tools/gensites2.py` still lists RenderScene #2 (and the earlier removed rows).
- SE was not run; every SE id and offset comes from the SE disassembly of DrawFrame, RenderScene and RenderPlayerView.
- AE 1.7.99 offsets are not supported by the new call site (as SideEffects).
