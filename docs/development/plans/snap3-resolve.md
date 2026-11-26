# snap3-resolve: the RenderWorld resolve points and the sun query (phase 3)

Lead: `resolve`, branch `snap3/resolve` (base `org_integration` 5986a3c4b), stage `s3-resolve`. Phase 3 of
[render-thread-snapshots.md](render-thread-snapshots.md); decisions 8, 57, 66-70, 87-110 of
[render-thread-decisions.md](render-thread-decisions.md); starting point `snap3-rworld.md` section 2c.

| Item | State | Commit |
|---|---|---|
| 1. Pre/post-resolve CS points fed from CS's frame | partly done: sky, water mask and surface, sun and StartDeferred's forward targets read `WorldPass` | 73c3640cd (sky in cdcc35bfa) |
| 2. Stop calling the host engine functions | partly done: ClearItemsList and RenderFirstPersonView_End dropped; StartAccumulating and the post-resolve still called | cdcc35bfa |
| 3. Sun occlusion query: render side draws, game side consumes | done | cdcc35bfa |
| 4. Detector numbers | below | |

## 3. The sun occlusion query

- **Render side** (`Native::RenderWorld`, the engine's step 12): `Sky::SubmitSunQuery` draws the sun's occlusion pass, the same draw
  as the graph's port of FUN_1414b4c00 (`DrawSun`, shared with the engine-path thunk), into a CS-owned query slot
  (`OcclusionQueries::kSunSlot`, slot 64, past the engine's ring). The slot is tagged with the snapshot id. No engine query object, no
  BeginQuery/EndQuery, no `Sun::SubmitOcclusionQuery`. A new draw waits until the game side took the last answer, as the engine waits
  on its pending entry. The geometry's hidden flag is not toggled (the engine clears it around its draw; the port's draw ignores it).
- **Game side** (`SunQuery.cpp`, a snapshot capture, native world snapshots only): a port of `Sun::UpdateOcclusionQueries`
  (SE 25801, AE 26355). For the main accumulator's query 1 it reads CS's slot (`OcclusionQueries::Take`, never waits). Other
  entries (the first-person view's, still the engine's path) read their ring slot (`ReadRingEntry`). It writes the entries, the
  lowest count, the pending flag and the visibility at accumulator+0x68 as the engine does. It then captures `BlockId::SunQuery`
  (the submit gate, sun+0x4C, and the held query geometry). It also sets the sun's accumulator to the main one, as
  SubmitOcclusionQuery would. The sample weight is read from the engine's constant (SE 304462, AE 254682).
- **Result:** answers arrive about 1.9 snapshots after the draw (river run). Every answer in every config was 0 samples, both before
  (engine path, `s3-rworld-*`) and after. No test scene shows the sun, so the glare value itself is unverified (open).

## 2. The accumulator's end calls

An observe phase counted, at the start and end of every native world render, the main accumulator's back-to-front list, its
first-person list and its batch renderer's pass groups. Over quick and river (1,800+ renders): never anything. ClearItemsList
(SE 99952, AE 106597) and RenderFirstPersonView_End (SE 99964, AE 106610) are no longer called.

## 1. The points read CS's frame (`RenderGraph/Engine/Native/WorldPass.{h,cpp}`)

`WorldPass::RenderWorldScope` marks native's RenderWorld. Each point opens a `WorldPass::Scope` (SkyBatch, WaterMask,
WaterSurface, SunQuery) and its readers ask `WorldPass::Current()`: targets, depth target and the render modes, as the engine's
resolve functions leave them for the main accumulator. Converted readers: Sky `CurrentShape` and `Modes::Current`, `SunShape`,
Water `CurrentShape`, `Frame::Read`'s mask targets and `Modes::Current`, and Deferred::StartDeferred's forward targets
(`WorldPass::ForwardTargets`: kMAIN, kMOTION_VECTOR, the normals swap target by the engine's rule, none). Guest and the
reflection faces keep the shadow state.

Observe phase: a shadow-state signature per point (quick), then CS-derived against engine-set values at each read (river):
0 differences of 500-3,500 per point. Scaffolding removed.

Not converted, and why:
- Forward's `frameBufferCached.GetFrameParams().z`: engine 1, `FrameCamera::Main()` 0. The perFrame buffer comes from
  SetCameraData and AE 0x140e45680 (the post-resolve's camera constants, called four times); no CS producer yet.
- DCLF BeforeOpaquePass: `SetDirtyStates` then `CaptureBindings` reads the engine context's bound CBs, SRVs, targets and
  viewport (bound state). Needs DCLF's colour epoch to take its bindings from CS's frame.
- Sky/water `Frame::Read`: `rss.viewPort`, alpha and stencil refs, the State depth range (written by the pre-resolve's LOD
  near-plane helper AE 0x1414cb3b0 / 0x1414cb510), water `Port::CaptureFrame`'s engine globals.
- The reflections pass and the water blend (ImageSpace) call SetCameraData, SetRenderTarget and read the current camera.
- RenderWorld's ImageSpace copy or snow pass: the copy step's rows come from the engine effect's parameters that
  `copySources`/`copyRegion` set; a native step needs explicit rows.

## Why the resolve functions are still called (item 2, open)

Replacing StartAccumulating and the post-resolve with CS functions needs, besides the CS points (now callable with CS's state),
CS producers for what their engine calls still do in native: the perFrame/camera constants (SetCameraData x2, AE 0x140e45680 x4),
the pending render target state flush and its clears (SetDirtyStates, ApplyState x2, ClearDepthStencil, which the skeleton turns
into graph clears), the LOD near-plane camera tweak, and the shadow-state writes later engine code (RenderPostProcess) reads after
RenderWorld. The batch calls themselves draw nothing (the accumulator is empty, observed).

## Detector (per native world frame)

Engine render calls: quick 57 (56 before, the pre/post-resolve sites run 1.88-1.91 a frame with the scene), interior 38 (38),
screen blood 47 (47), river 41 (41), tour 39 (39). The detector's count does not move: every call removed was a CS direct call
(decision 57). Removed CS direct calls per world frame: Sun::UpdateOcclusionQueries, Sun::SubmitOcclusionQuery, ClearItemsList,
RenderFirstPersonView_End (4).

CS direct calls to engine render functions still made from RenderWorld's scope (not counted by decision 57):
StartAccumulating (SetCameraData, the accumulator's setup, the pre-resolve body with 21 engine call sites), the post-resolve body
(20 sites), SetRenderTarget x4-6, the ImageSpace copy (`copySources`, `copyRegion`, `DispatchCopySubRegion`) or `RenderSnow`,
RunReflections' SetCameraData/SetRenderTarget/SetDepthStencil, RunSsao's and RunWaterBlend's SetRenderTarget/SetDepthStencil,
DrawSun's SetCameraData, BeforeOpaquePass's SetDirtyStates.

Live engine reads: quick 778 (777 before this branch's switch build), river 693.

## Runs (stage `s3-resolve`, Dev-Fast, detector on)

| Config | Label | Result |
|---|---|---|
| native-vulkan+quick, observe | `s3-resolve-a-nvk-observe` | passed; signatures per point, accumulator empty, first sun answers |
| native-river, parity | `s3-resolve-b-river-parity` | passed; 0 differences at every point |
| guest+quick | `s3-resolve-f-guest-quick` | passed (67) |
| native-vulkan+quick | `-f-nvk-quick` | passed (57) |
| native-d3d12-quick | `-f-d3d12-quick` | passed (53) |
| native-vulkan+screen-blood | `-f-blood` | passed (47) |
| native-vulkan+cadence-drop, -drop-cells, -repeat | `-f-drop`, `-f-drop-cells`, `-f-repeat` | passed (55, 46, 42) |
| native-vulkan+shadows-blend | `-f-shadows-blend` | passed (43) |
| native-interior | `-f-interior` | passed (38) |
| native-river | `-f-river` | passed (41) |
| native-vulkan+post-effects | `-f-post-effects` | passed (42) |
| native-vulkan+lens | `-f-lens` | passed (42) |
| native-tour | `-f-tour` | passed (39) |
| cadence.py, cadence-still | `-f-cad-*` | all passed; exterior repeat 16.6 / drop 14.8 against a floor of 1.6 (the same sizes as snap3-rworld's 16.6 / 14.1 and snap3-culls': wind and pose), interior within noise |

## Decisions (agent, to confirm)

- **RS-a.** The sun query uses a CS-owned slot past the engine's ring, not an engine query object; the game side reads it in a
  snapshot capture (decision 8: UpdateOcclusionQueries is game-side logic, ported).
- **RS-b.** The game side sets the sun's accumulator to the main one when it captures a submit, as SubmitOcclusionQuery did.
- **RS-c.** The sun query geometry is read on the render thread (held by the block, checked by key), like decision 105.
- **RS-d.** WorldPass values are constants of the engine's resolve code for the main accumulator, not per-frame captures:
  they do not depend on the scene (checked over two configs). The normals target follows the engine's rule.
- **RS-e.** Item 2's StartAccumulating/post-resolve replacement was left for its own item once the four producers above exist,
  instead of a CS copy of the engine bodies that would keep calling the same engine functions.

## Proposed decision-log entries

- **Native's sun occlusion query is CS's:** the render side draws it into a CS-owned query slot tagged with the snapshot id; the
  capture ports Sun::UpdateOcclusionQueries on the game thread and takes the answer (never waits). The engine's query functions
  are not called in native. Guest unchanged.
- **The main accumulator's end calls are not made in native** (ClearItemsList, RenderFirstPersonView_End): its lists are empty.
- **RenderWorld's points read CS's frame (WorldPass)** for targets, depth target and modes at the sky batch, the water mask and
  surface and the sun draw, and StartDeferred's forward targets; remaining readers and the producers item 2 needs are listed above.

## Open items

- Item 2 (above) and the unconverted readers (section 1).
- The sun glare value is untested with the sun in view (no test config shows it).
- SE untested (new addresses are SE/AE pairs: the sample weight 304462/254682, the sun 528089/415034).
