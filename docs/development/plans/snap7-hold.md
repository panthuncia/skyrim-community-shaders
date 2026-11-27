# snap7-hold: DCLF's render work without the engine-read window's hold

Lead `hold`, round `snap7`, branch `snap7/hold` (base `org_integration` ca9035fe0), stage `s7-hold`. Item: DCLF's `EngineReadWindow`
render-half hold (decision 140) and the render-side engine reads it covered; snap7-ticket's open items (the producer split, scene
pairing, `coveredLast` and the family decision as messages, DCLF's other points). Still today's single-threaded frame: no render thread.

Status: **done** for the hold (gone) and the render-side reads listed in section 2; what is left, with designs, is section 5.

## 1. The window without `kHeld`

The hold never protected the render side's own reads: the render side takes no lease. It kept DCLF's workers out of engine memory while
the engine's DrawFrame writes it outside Main::Draw to Present (the decal manager's attaches before Main::Draw, the next update's jobs
after Present). Those writes are game-side work, in phase 7 too. So the window is now simply open exactly while the engine only reads:

- `EngineReadWindow::Open()` in DCLF's game half (`FrameDriver::GameFrameStart`, at SetupSunAndSky, or a dropped update's game half);
  it was `ReleaseRenderHalf`.
- `EngineReadWindow::CloseAtPresent()` in `FrameDriver::EndFrame` (main thread inside a game update only, as the hold was); it was
  `HoldForRenderHalf`.
- `Close()` at the end of Main::Update's call of Main::DrawFrame, as before (usually already closed).
- The open after the capture and the hold at each DrawFrame call (`Publication.cpp`) are gone: the window opened there was held at
  once. `kHeld`, `HoldForRenderHalf`, `ReleaseRenderHalf`, `renderHalfHeld` and the "render-half holds" count are deleted.

Leases are taken in the same interval as before (game half to Present). A repeated render has no game half, so its window stays closed
(it was held). A dropped update opens it in its game half (it was open from the capture; nothing ran between). Menu and loading frames
never open it (they never released the hold). In phase 7 `CloseAtPresent` moves to the game thread's publish, where the engine starts
the next update's jobs; the render thread never touches the window.

What still takes leases outside the game half is only the frame values' producer (kicked by the render half's `FrameValues::Kick`,
running beside the render half). That is the producer split (section 5).

## 2. What moved out of the render half

Carriers: the frame ticket (`FrameTicket::parts`, a new per-feature slot taken in the game half), the adopted snapshot (menus), the game
half's end in the native driver, the update's game-side end (new `Snapshot::RegisterUpdateEnd`), and a snapshot capture.

| Site | What it read | Now |
|---|---|---|
| `WaterNative.cpp:675` ScenePasses (Data, world root) + every scene pass's BuildPass (geometry, property, material) | the tracked water shapes, their reach, bounds, cached stencil pass, sub-group, port object values, material bytes, renderer data, Light Limit Fix room | `TicketPart::WaterScene` (`SceneShapes`, native): the shapes under the world root, not hidden, with `PassObject` values; ScenePasses filters by claims and frustum and derives passes from the values; BuildPass reads only `PassObject` |
| `SkyNative.cpp:1035` RootPasses (Sky, sky root walk) | the sky root's objects, flags, bounds, passes | `TicketPart::SkyRoot` (`RootObjects`, both modes: the reflection faces cull it in guest too): the root flattened in pre-order (hidden subtrees left out), each with bound, always-draw, kind, pass id and sub-group; the face/main cull walks the values |
| `FrameView.cpp:291` from EndFrame's `NoteLoadingScreen`, `LoadShown` (LoadHold's re-entry and undrawable checks) | the live loading menu | `FrameView::LoadingScreenInFrame()`: native reads the adopted snapshot's `menus.loading` (a loading screen's own frame-only snapshot on the serving thread); `NoteLoadingScreen(bool)` takes the frame's value (the game half passes the ticket's `loading`) |
| `LoadHold.cpp:152` MenuOpen, `:133` ActorsWithout3D (frame end) | the loading menu, the high process list | `MenuOpenInFrame` (snapshot menus in native), `ActorsWithout3DInFrame` (counted in `LoadHold::OnFrameStart`, the game half, in native) |
| `Skin Surfaces.cpp:122` via `Skin::Prepass` (record) | each entry's owner: the actor's 3D walk, the player, the property | `Skin::RevalidateOwners` at a snapshot capture (`Skin::Capture`, native); Prepass reads the entry's `stale` mark. Guest checks at Prepass as before |
| DepthCommitted `LatePoses::ConfirmFrame` (uncounted root walk) | the late-pose roots | native: `FrameDriver::GameHalfEnd` at the end of the driver's game half (`RenderScene.cpp`, before Main::Draw's tail), every render; DepthCommitted uses the result. Guest unchanged |
| BeforePresent `LatePoses::CheckAtPresent` (uncounted walk) | the late-pose roots | native: `FrameDriver::UpdateEnd` (game side, after the renders). Guest unchanged |
| EarlyPrepass `ServeMaterialRequests` (`property->material`, material capture) | the requested properties' materials | native: the game half (`GameFrameStart`, after the claims). Guest at EarlyPrepass |
| `ApplyEvents.cpp:412` (coordinator worker, live UI) | the loading menu | `Channels::loadingSeen` and the load hold's state |
| `WholeCull.cpp` diagnostic | the loading menu | `LoadingScreenInFrame` |

Left as render-owned (snap7-reads RD-a): Prepass's `LatchAccumulator` (the main accumulator global, Accumulator), BeforeOpaquePass's
`ApplyPendingState` (renderer state). EndFrame's `PrimaryCull::EndFrame` moves the list graveyard to the engine lane without reading it.

### Messages from the render side to the game half

- `FrameDriver::coveredLast` is an `std::atomic<bool>`: the render half stores each render's coverage (and `NoteStoppedDrawing` clears
  it at a revision's `left`); the game half loads the latest when it installs claims. It never waits for a newer one.
- The draw families' decision (game half) read the adopted revision's family pipelines (`AdoptedFamilyPipelines`, render-side state).
  Now the render side publishes them after each adoption (`IndirectDraws::PublishFamilyRenderState`, from the scene source's
  `afterAdoption`) into an atomic `FamilyRenderState`, and `DecideDrawFamilies` takes the latest once per decision (`FamilyInputs`).
  The frame catalog stays the game half's read: it is the installed publication's, which the game half installs (`HandOverAtFrameStart`).
  In today's frame the published value is this Begin's, so decisions are unchanged.

## 3. Detector

New object `Scene` (`EngineReads::Object::Scene`, game-mutated): a walk of the scene graph, counted once per walk with
`CS_ENGINE_READ_MARK(Scene, ...)`: water BuildPass per pass (both paths), water scene capture, sky RootPasses (before) / sky root
capture (after), sky BuildPass per pass, LatePoses' root search, ServeMaterialRequests per request.

## 4. Numbers and runs

Stage `s7-hold`, Dev-Fast, `CS_SNAPSHOT_DETECT=1` on native runs. Base: org_integration ca9035fe0 plus the `Scene` marks (first commit).

Render half, live reads of game-mutated objects per world frame (native-vulkan+quick):

| | base (s7-hold-base-nvk-quick) | after (s7-hold-a-nvk-quick) |
|---|---|---|
| record, game-mutated | 78.7 | 26.0 |
| frame end, game-mutated | 8.8 | 6.7 |
| whole run, live reads per world frame | 667 | 590 |

Per DCLF site (record unless named):

| Site | Object | base | after |
|---|---|---|---|
| WaterNative.cpp BuildPass (Scene mark, per pass) | Scene | 42.42 | 0 |
| WaterNative.cpp:674/677 ScenePasses (walk, world root) | Scene, Data | 3.00 + 3.00 | 0 (captures) |
| SkyNative.cpp:1036/1049 RootPasses (sky, walk) | Sky, Scene | 1.00 + 1.00 | 0 (captures) |
| FrameView.cpp:291 | UI | 1.00 | 0 |
| FrameView.cpp:291 (frame end) | UI | 1.74 | 0 |
| LoadHold.cpp:152 / :133 (frame end) | UI, Data | 0.26 / 0.01 | 0 |
| Skin Surfaces.cpp:122 (native-tour: Dragonsreach, Whiterun) | Player | 0.82, interior 6.4 (snap7-reads) | 0 |
| SkyNative.cpp:534 sky BuildPass (Scene mark, per pass) | Scene | 20.91 | 20.93 (left, section 5) |

Left in the record half (not DCLF scene work): precipitation (Skylighting.cpp:819/873, OccluderRules.cpp:82, WetnessEffects.cpp:984;
snap7-precip), PostChain.cpp:402 (0.02, ImageSpace). Frame end: the engine fixes' and the harness's reads (snap7-frameend). The
late-pose walks and material requests are not exercised by quick (Improved Camera is off in the harness; few requests in a short run);
in native they are game-half code now.

| Config | Label | Result |
|---|---|---|
| native-vulkan+quick | s7-hold-base-nvk-quick | passed (base) |
| native-vulkan+quick | s7-hold-a-nvk-quick | passed with the shot reference |
| native-vulkan+native-river | s7-hold-a-nvk-native-river | passed (water and reflections drawn) |
| native-vulkan+taa-water | s7-hold-a-nvk-taa-water | passed |
| native-vulkan+sky-native | s7-hold-a-nvk-sky-native | passed (4 shots: sky, clouds, night) |
| native-vulkan+screen-blood | s7-hold-a-nvk-screen-blood | passed |
| native-vulkan+native-tour | s7-hold-a-nvk-native-tour | passed (3 shots) |
| native-vulkan+cadence-drop-cells | s7-hold-a-nvk-cadence-drop-cells | passed (3 shots) |
| native-vulkan+update-cap30 | s7-hold-a-nvk-update-cap30 | passed |
| native-vulkan+update-cap240 | s7-hold-a-nvk-update-cap240 | passed |
| guest+quick | s7-hold-a-guest-quick | passed |

Skipped: native-d3d12-quick (no ORG, BasicRHI or native device code changed); first-person (the late poses need Improved Camera, which
the harness turns off, so no config exercises them).

## 5. Left, with designs

1. **Sky BuildPass's per-pass reads** (`SkyNative.cpp` BuildPass and DrawRootPasses, 20.9 Scene marks a world frame): each pass reads
   its property (object type, blend colour and value, fade flag, cloud layer, base and blend textures, the flags at +0x38), its
   geometry (world and previous world at +0x7C/+0xB0, the alpha property's test bit, renderer data, triangle count) and three engine
   globals (VParams, cloud offsets, sky colours); CS's sky hooks (`State::UpdateSkyShaderPermutation`, `Effects11::ModifySky`,
   `CloudShadows::ModifySky`) read the property's `uiSkyObjectType` and use the geometry as a key. Design: a `PassObject` like the
   water's in each `RootObject` of kind Pass (and the three globals in `RootObjects`), filled in the game half; BuildPass reads only it;
   the engine-batch path (BuildBatch) fills it from its pass; the three hooks get overloads taking the object type and the geometry key.
   The list (RootPasses) already reads only the ticket.
2. **The producer split** (snap7-ticket open item 1). The frame values' producer is one ordered job per render (`FrameValues::Kick`):
   `Sample` (mover keys, the due movers' and roots' rows under leases, shading rows, registered pools), then `Upload` into the render's
   leased backings, then `StoreForwardRows` and the spheres for the CPU frame slot. It is the only lease taker kicked from the render
   half. Design: two job kinds on the same ordered producer chain (rows are never touched by two jobs at once, no lock): a sample job
   per ticket, kicked by the game half with the ticket's one-shot inputs, which writes the kept row arrays and their journals and takes
   every lease; an upload job per render, kicked by the render half, which trims the journals to its backings' floor (`BeginBuild` moves
   here), uploads, signals the frame wait value and fills the CPU frame slot. A repeated render kicks an upload job only (its sampling is
   refused today anyway, TICKET-f). `seq` (the GPU wait value) stays per render; the sample jobs get their own order. Then, in phase 7,
   the window can close at the end of the game half.
3. **Scene pairing** (snap7-ticket open item 2). Today the revision adopts DCLF's newest posted scene at Begin (render side), before the
   game half, and the game half installs that publication's claims. Design for phase 7: the game half picks the newest posted publication
   into the ticket (`FrameSnapshot::scene`) and installs its claims; the scene source's `takes` becomes "the scene of the ticket this
   render adopts" (a repeat keeps it, as `HoldsScene` does now). It needs the revision to adopt a named scene instead of the newest (a
   change in `Revision.cpp`, shared) and a ticket made before its render's Begin, which only the render thread gives.
4. **Render-to-game state left.** The draw families' `Pipelines()` still asks the pipeline lane for the slot's pipelines
   (`DrawPipelines::SetFamilyInputs`, `RequestFamily`, `NoteFamilyWait`) from the game half; first person's depth import and
   `PassCapture`/`ParticleSnapshots` (snap7-ticket open item 4) as before. With a render thread, a family decision made against an
   older adopted revision than the render's must be checked by the render half (coverage already is).
5. **Lifetimes.** The ticket's water shapes and sky objects are keys plus raw renderer-data and texture pointers that BuildPass still
   follows (the geometry's buffers, the material's textures' views). Today the render half runs before anything can release them. With
   a render thread they need the retire queue's keep-alive.

## 6. Decided by the agent, to confirm

- HOLD-a. **The window is open from DCLF's game half to Present, without a hold bit.** The hold protected game-side engine writes (decal
  attaches, the next update's jobs), not the render side's reads; opening at the game half and closing at Present gives the same lease
  interval. In phase 7 `CloseAtPresent` moves to the game thread's publish.
- HOLD-b. **Native's late poses are confirmed at the end of the driver's game half** (before Main::Draw's tail) instead of the depth
  commit: native's render half calls no engine code, so the scene graph is the same at both points. Guest keeps the depth commit.
- HOLD-c. **Native answers material requests in the game half**, so a request a join posts after the frame start waits up to one update
  longer (it was answered at EarlyPrepass). Guest unchanged.
- HOLD-d. **The sky root is captured in both modes** (guest's reflection faces cull it too); guest reads it at SetupSunAndSky instead of
  at the face render, within the same DrawFrame.
- HOLD-e. **Native's render-side load checks read the adopted snapshot's menus** (EndFrame's `NoteLoadingScreen`, LoadHold, the re-entry
  checks): a load opened inside a render is seen one Present later at the frame end; the next game half sees it live, and the window is
  closed during the load anyway.
- HOLD-f. **The draw families read the adopted revision from a message published at the adoption** (`FamilyRenderState`); the catalog
  stays a game-half read (the game half installs it).
- HOLD-g. **Skin's owner checks run at the snapshot capture in native** (once per update instead of once per Prepass). Guest unchanged.

## 7. Proposed decision-log entries

- **The engine-read window has no render-half hold** (refines 140): it opens in DCLF's game half and closes at Present (main thread
  inside an update) or at the end of the DrawFrame call; `kHeld`, `HoldForRenderHalf`, `ReleaseRenderHalf` are gone. The only lease
  taker kicked from the render half is the frame values' producer (the producer split).
- **DCLF's render half reads its scene work from the frame ticket** (`FrameTicket::parts`, `RegisterTicketPart`): the tracked water
  shapes with their port values (`TicketPart::WaterScene`) and the sky root flattened (`TicketPart::SkyRoot`). Native's load checks read
  the snapshot's menus; late poses, material requests (native) and Skin's owner checks moved to the game side (`FrameDriver::GameHalfEnd`,
  `UpdateEnd` via the new `Snapshot::RegisterUpdateEnd`, a Skin capture). `coveredLast` and the adopted family pipelines are
  render-to-game messages. Record-half game-mutated reads in quick 78.7 -> 26.0 (DCLF's left: sky BuildPass 20.9); DCLF's frame-end
  reads 2.0 -> 0.
- **The detector counts scene-graph walks** (`EngineReads::Object::Scene`, once per walk).
- **Left:** sky BuildPass's per-pass values, the producer split, scene pairing (designs above).
