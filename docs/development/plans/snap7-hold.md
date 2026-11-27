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

(filled in below)

## 5. Left, with designs

(filled in below)

## 6. Decided by the agent, to confirm

(filled in below)
